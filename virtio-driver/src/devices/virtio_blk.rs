// SPDX-License-Identifier: (MIT OR Apache-2.0)

use crate::virtqueue::VirtqueueLayout;
use crate::{ByteValued, Completion, Le16, Le32, Le64, VirtioFeatureFlags, VirtioTransport};
use bitflags::bitflags;
use libc::{c_void, iovec, EIO, ENOTSUP, EPROTO};
use std::convert::TryFrom;
use std::io::{Error, ErrorKind};
use std::iter;
use std::mem;

// TEST
use crate::rustvq::{
    self,
    adapter,
    userspace_hal::UserspaceHal,
    virtqueue::{
        BufferDirection, DescriptorLayout, DmaSegment, Token,
        VirtQueue, VirtQueueFeatures,
    },
};

use crate::IovaTranslator;
use std::ptr::{self, NonNull};
//END TEST

bitflags! {
    pub struct VirtioBlkFeatureFlags: u64 {
        const SIZE_MAX = 1 << 1;
        const SEG_MAX = 1 << 2;
        const GEOMETRY = 1 << 4;
        const RO = 1 << 5;
        const BLK_SIZE = 1 << 6;
        const FLUSH = 1 << 9;
        const TOPOLOGY = 1 << 10;
        const CONFIG_WCE = 1 << 11;
        const MQ = 1 << 12;
        const DISCARD = 1 << 13;
        const WRITE_ZEROES = 1 << 14;
        const LIFETIME = 1 << 15;
        const SECURE_ERASE = 1 << 16;
    }
}

/// The Device Configuration Space for a virtio-blk device.
///
/// This is `struct virtio_blk_config`` from the VIRTIO 1.1 specification (see 5.2.4).
#[derive(Clone, Copy, Default)]
#[repr(C, packed)]
pub struct VirtioBlkConfig {
    pub capacity: Le64,
    pub size_max: Le32,
    pub seg_max: Le32,
    pub cylinders: Le16,
    pub heads: u8,
    pub sectors: u8,
    pub blk_size: Le32,
    pub physical_block_exp: u8,
    pub alignment_offset: u8,
    pub min_io_size: Le16,
    pub opt_io_size: Le32,
    pub writeback: u8,
    _unused0: u8,
    pub num_queues: Le16,
    pub max_discard_sectors: Le32,
    pub max_discard_seg: Le32,
    pub discard_sector_alignment: Le32,
    pub max_write_zeroes_sectors: Le32,
    pub max_write_zeroes_seg: Le32,
    pub write_zeroes_may_unmap: u8,
    _unused1: [u8; 3],
}

unsafe impl ByteValued for VirtioBlkConfig {}

fn to_lba(offset: u64) -> Result<u64, Error> {
    // This is independent of the reported block size of the device
    let block_size = 512;

    if offset & (block_size - 1) != 0 {
        return Err(Error::new(ErrorKind::InvalidInput, "Unaligned request"));
    }

    Ok(offset / block_size)
}

pub fn validate_lba(offset: u64) -> Result<(), Error> {
    to_lba(offset).map(|_| ())
}

pub fn virtio_blk_max_queues(transport: &VirtioBlkTransport) -> Result<usize, Error> {
    // Some transports (e.g. vhost-vdpa before Linux v5.18) may not be able
    // to provide the number of queues, so let's look in the config space.
    let features = VirtioBlkFeatureFlags::from_bits_truncate(transport.get_features());
    if features.contains(VirtioBlkFeatureFlags::MQ) {
        let cfg = transport.get_config()?;
        Ok(u16::from(cfg.num_queues) as usize)
    } else {
        // If VirtioBlkFeatureFlags::MQ is not negotiated, the device supports
        // only a single queue
        Ok(1)
    }
}

#[derive(Clone, Copy, Default)]
#[repr(C, packed)]
#[allow(dead_code)]
struct DiscardWriteZeroesData {
    sector: Le64,
    num_sectors: Le32,
    flags: Le32,
}

bitflags! {
    pub struct DiscardWriteZeroesFlags: u32 {
        const UNMAP = 1 << 0;
    }
}

impl DiscardWriteZeroesData {
    fn new(offset: u64, len: u64, unmap: bool) -> Result<Self, Error> {
        let start = to_lba(offset)?;
        let num_sectors = u32::try_from(to_lba(len)?)
            .map_err(|_e| Error::new(ErrorKind::InvalidInput, "Discard length too large"))?;
        let flags = if unmap {
            DiscardWriteZeroesFlags::UNMAP.bits()
        } else {
            0
        };

        Ok(DiscardWriteZeroesData {
            sector: start.into(),
            num_sectors: num_sectors.into(),
            flags: flags.into(),
        })
    }
}

/// The request header for virtio-blk devices.
///
/// This is the first part of `struct virtio_blk_req`` from the VIRTIO 1.1 specification (see
/// 5.2.6).
#[derive(Clone, Copy, Default)]
#[repr(C, packed)]
#[allow(dead_code)]
struct VirtioBlkReqHeader {
    req_type: Le32,
    _reserved: Le32,
    sector: Le64,
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
enum VirtioBlkReqType {
    Read = 0,
    Write = 1,
    Flush = 4,
    Discard = 11,
    WriteZeroes = 13,
}

impl VirtioBlkReqType {
    fn is_from_dev(&self) -> bool {
        // The return value for Flush doesn't matter because it doesn't have any buffers
        *self == Self::Read
    }
}

impl VirtioBlkReqHeader {
    fn new(req_type: VirtioBlkReqType, offset: u64) -> Self {
        Self {
            req_type: (req_type as u32).into(),
            _reserved: 0.into(),
            sector: offset.into(),
        }
    }
}

unsafe impl ByteValued for VirtioBlkReqHeader {}

#[derive(Clone, Copy)]
pub struct VirtioBlkReqBuf {
    header: VirtioBlkReqHeader,
    status: u8,
    dwz_data: DiscardWriteZeroesData,
}

pub type VirtioBlkTransport = dyn VirtioTransport<VirtioBlkConfig, VirtioBlkReqBuf>;

/// A queue of a virtio-blk device.
///
/// This is used to send block I/O requests to the device and receive completions. Note that calling
/// transport specific functions may need to be called before or after certain operations on the
/// `VirtioBlkQueue`:
///
/// * All request methods only enqueue the requests in the rings. They don't notify the device of
///   new requests, so it may or may not start processing them. Call
///   [`crate::QueueNotifier::notify`] on the result of [`VirtioTransport::get_submission_notifier`]
///   after queuing requests to notify the device. You can queue multiple requests and then send a
///   single notification for all of them.
///
/// * To be notified of new completions, use the `EventFd` returned by
///   [`VirtioTransport::get_completion_fd`].
///
/// When a request is submitted, the user provides a "context" of type `C` that will later be
/// returned in the completion for that request.
///
/// Use [`setup_queues`] to create the queues for a device.
///
/// # Examples
///
/// ```no_run
/// # use virtio_driver::{
/// #     VhostUser, VirtioBlkQueue, VirtioBlkTransport, VirtioFeatureFlags, VirtioTransport
/// # };
/// use rustix::fs::{memfd_create, MemfdFlags};
/// use std::ffi::CStr;
/// use std::fs::File;
/// use std::os::unix::io::{AsRawFd, FromRawFd};
/// use std::sync::{Arc, RwLock};
///
/// // Connect to the vhost-user socket and create the queues
/// let mut vhost = VhostUser::new("/tmp/vhost.sock", VirtioFeatureFlags::VERSION_1.bits())?;
/// let mut vhost = Arc::new(RwLock::new(Box::new(vhost) as Box<VirtioBlkTransport>));
/// let mut guard = vhost.write().unwrap();
/// let mut queues = VirtioBlkQueue::<&str>::setup_queues(&mut **guard, 1, 128).unwrap();
///
/// // Create shared memory that is visible for the device
/// let mem_file: File = memfd_create("guest-ram", MemfdFlags::empty())?.into();
/// mem_file.set_len(512)?;
/// let mut mem = unsafe { memmap2::MmapMut::map_mut(&mem_file) }?;
/// guard.map_mem_region(mem.as_ptr() as usize, 512, mem_file.as_raw_fd(), 0)?;
/// drop(guard);
///
/// // Submit a request
/// queues[0].read(0, &mut mem, "my-request-context")?;
/// vhost.read().unwrap().get_submission_notifier(0).notify()?;
///
/// // Wait for its completion
/// let mut done = false;
/// while !done {
///     let ret = vhost.read().unwrap().get_completion_fd(0).read();
///     if ret.is_err() {
///         continue;
///     }
///
///     for c in queues[0].completions() {
///         println!("Completed request with context {:?}, return value {}", c.context, c.ret);
///         done = true;
///     }
/// }
/// # Result::<(), Box<dyn std::error::Error>>::Ok(())
/// ```
///
/// [`setup_queues`]: Self::setup_queues
pub struct VirtioBlkQueue<C> {
    // TEST
    vq: VirtQueue<UserspaceHal>,

    iova_translator: Box<dyn IovaTranslator>,
    req_bufs: NonNull<VirtioBlkReqBuf>,

    req_contexts: Box<[Option<C>]>,
    // END TEST
}

// TEST
// SAFETY:
// - All operations that access or mutate the virtqueue, request
//   buffers, or request contexts require exclusive &mut self.
// - The underlying pointers are not exposed through shared
//   references to VirtioBlkQueue.
// - Moving the queue does not invalidate the pointed-to memory.
// - The transport must keep the registered backing memory alive
//   until the queue is dropped.
// - Backend access to shared ring memory is synchronized through
//   the VirtIO protocol and the HAL memory barriers.
//
// This does not make VirtQueue or UserspaceHal independently
// thread-safe.
unsafe impl<C: Send> Send for VirtioBlkQueue<C> {}

// SAFETY:
// - Shared access to VirtioBlkQueue does not permit mutation of
//   the internal Cell, descriptor state, or request buffers.
// - Operations requiring such mutation need &mut self.
// - Request contexts are Sync, and the IovaTranslator already
//   requires Send + Sync.
unsafe impl<C: Send + Sync> Sync for VirtioBlkQueue<C> {}
// END TEST

impl<C> VirtioBlkQueue<C> {
    fn new(
        vq: VirtQueue<UserspaceHal>,
        iova_translator: Box<dyn IovaTranslator>,
        req_bufs: NonNull<VirtioBlkReqBuf>,
    ) -> Self {
        let queue_size = usize::from(vq.size());
        let req_contexts = iter::repeat_with(|| None).take(queue_size).collect();

        Self {
            vq,
            iova_translator,
            req_bufs,
            req_contexts,
        }
    }

    /// Creates the queues for a virtio-blk device.
    pub fn setup_queues(
        transport: &mut VirtioBlkTransport,
        num_queues: usize,
        queue_size: u16,
    ) -> Result<Vec<Self>, Error> {
        if virtio_blk_max_queues(transport)? < num_queues {
            return Err(Error::new(
                ErrorKind::InvalidInput,
                "Too many queues requested",
            ));
        }

        let features = VirtioFeatureFlags::from_bits_truncate(transport.get_features());

        // TEST
        if features.contains(VirtioFeatureFlags::RING_PACKED) {
            return Err(Error::new(
                ErrorKind::Unsupported,
                "RFL POC supports split virtqueues only",
            ));
        }
        // END TEST

        let layout =
            VirtqueueLayout::new::<VirtioBlkReqBuf>(num_queues, queue_size as usize, features)?;
        let queues: Vec<Self> = {
            // Not actually needless: must drop the borrow on the transport before alloc_queue_mem()
            #[allow(clippy::needless_collect)]
            let iova_translators: Vec<_> = iter::repeat_with(|| transport.iova_translator())
                .take(num_queues)
                .collect();

            let mem = transport.alloc_queue_mem(&layout)?;

            // TEST
            let mut queues = Vec::with_capacity(num_queues);

            for (i, iova_translator) in
                iova_translators.into_iter().enumerate()
            {
                let start = i * layout.end_offset;
                let end = start + layout.end_offset;
                let mem_queue = &mut mem[start..end];

                // The first part of each registered queue region
                // is the backing arena for OUR split virtqueue.
                let ring_len = layout.req_offset;

                let cpu_base = NonNull::new(mem_queue.as_mut_ptr())
                    .ok_or_else(|| {
                        Error::new(
                            ErrorKind::Other,
                            "RFL queue memory base is null",
                        )
                    })?;

                let dma_base = iova_translator
                    .translate_addr(
                        cpu_base.as_ptr() as usize,
                        ring_len,
                    )?
                    .0;

                // Per-request device-visible structures reside
                // after the ring arena.
                let req_size = mem::size_of::<VirtioBlkReqBuf>();
                let pool_size = req_size
                    .checked_mul(usize::from(queue_size))
                    .ok_or_else(|| {
                        Error::new(
                            ErrorKind::InvalidInput,
                            "RFL request pool size overflow",
                        )
                    })?;

                let pool_end = layout.req_offset
                    .checked_add(pool_size)
                    .ok_or_else(|| {
                        Error::new(
                            ErrorKind::InvalidInput,
                            "RFL request pool offset overflow",
                        )
                    })?;

                if pool_end > mem_queue.len() {
                    return Err(Error::new(
                        ErrorKind::InvalidInput,
                        "RFL request pool exceeds registered memory",
                    ));
                }

                let req_ptr = unsafe {
                    mem_queue.as_mut_ptr().add(layout.req_offset)
                };

                if (req_ptr as usize)
                    % mem::align_of::<VirtioBlkReqBuf>()
                    != 0
                {
                    return Err(Error::new(
                        ErrorKind::InvalidInput,
                        "RFL request pool is misaligned",
                    ));
                }

                let req_bufs = NonNull::new(
                    req_ptr.cast::<VirtioBlkReqBuf>()
                )
                .ok_or_else(|| {
                    Error::new(
                        ErrorKind::Other,
                        "RFL request pool address is null",
                    )
                })?;

                // The arena is a non-owning view. The transport
                // remains the owner of the backing mmap.
                let hal = unsafe {
                    UserspaceHal::new(
                        cpu_base,
                        dma_base,
                        ring_len,
                    )
                };

                let vq_features = VirtQueueFeatures {
                    indirect: false,
                    event_idx: features.contains(
                        VirtioFeatureFlags::RING_EVENT_IDX
                    ),
                };

                let vq = rustvq::new_virtqueue(
                    hal,
                    i as u16,
                    queue_size,
                    vq_features,
                )
                .map_err(|_| {
                    Error::new(
                        ErrorKind::Other,
                        "RFL VirtQueue creation failed",
                    )
                })?;

                eprintln!(
                    "RFL queue {i}: desc={:#x} avail={:#x} used={:#x}",
                    vq.descriptor_dma_addr().map_err(|_| {
                        Error::new(
                            ErrorKind::Other,
                            "Failed to get descriptor DMA address",
                        )
                    })?,
                    vq.driver_area_dma_addr().map_err(|_| {
                        Error::new(
                            ErrorKind::Other,
                            "Failed to get driver area DMA address",
                        )
                    })?,
                    vq.device_area_dma_addr().map_err(|_| {
                        Error::new(
                            ErrorKind::Other,
                            "Failed to get device area DMA address",
                        )
                    })?,
                );

                queues.push(Self::new(
                    vq,
                    iova_translator,
                    req_bufs,
                ));
            }

            queues
        };

        // Configure the transport using the addresses of OUR queues.
        let configs = queues
            .iter()
            .map(|queue| {
                adapter::transport_config(&queue.vq).map_err(|_| {
                    Error::new(
                        ErrorKind::Other,
                        "RFL transport config failed",
                    )
                })
            })
            .collect::<Result<Vec<_>, Error>>()?;
        // END TEST

        transport.setup_queues_from_config(&configs)?;

        Ok(queues)
    }

    fn queue_request_full(
        &mut self,
        req_type: VirtioBlkReqType,
        offset: u64,
        buf: &[iovec],
        dwz_data: Option<DiscardWriteZeroesData>,
        context: C,
    ) -> Result<(), Error> {
        let lba = to_lba(offset)?;

        // TEST
        // Choose a queue-local request slot.
        // Descriptor allocation remains entirely inside OUR VirtQueue.
        let slot = self.req_contexts
            .iter()
            .position(Option::is_none)
            .ok_or_else(|| {
                Error::new(ErrorKind::WouldBlock, "No free request slots")
            })?;

        let req_ptr = unsafe {
            self.req_bufs.as_ptr().add(slot)
        };

        // Initialize the entire device-visible request buffer.
        unsafe {
            ptr::write(
                req_ptr,
                VirtioBlkReqBuf {
                    header: VirtioBlkReqHeader::new(req_type, lba),
                    status: 0,
                    dwz_data: dwz_data.unwrap_or_default(),
                },
            );
        }

        let header_ptr = unsafe {
            ptr::addr_of_mut!((*req_ptr).header)
        };

        let status_ptr = unsafe {
            ptr::addr_of_mut!((*req_ptr).status)
        };

        let mut segments = Vec::with_capacity(buf.len() + 3);

        // The header is read by the device.
        let header_dma = self.iova_translator.translate_addr(
            header_ptr as usize,
            mem::size_of::<VirtioBlkReqHeader>(),
        )?;

        segments.push(DmaSegment::new(
            header_dma.0,
            mem::size_of::<VirtioBlkReqHeader>() as u32,
            BufferDirection::DriverToDevice,
        ));

        // DISCARD / WRITE_ZEROES command data.
        if dwz_data.is_some() {
            let dwz_ptr = unsafe {
                ptr::addr_of_mut!((*req_ptr).dwz_data)
            };

            let dwz_dma = self.iova_translator.translate_addr(
                dwz_ptr as usize,
                mem::size_of::<DiscardWriteZeroesData>(),
            )?;

            segments.push(DmaSegment::new(
                dwz_dma.0,
                mem::size_of::<DiscardWriteZeroesData>() as u32,
                BufferDirection::DriverToDevice,
            ));
        }

        // Data buffers supplied by the caller.
        for b in buf {
            if b.iov_len == 0 {
                continue;
            }

            let len = u32::try_from(b.iov_len).map_err(|_| {
                Error::new(
                    ErrorKind::InvalidInput,
                    "Buffer exceeds VirtIO descriptor length",
                )
            })?;

            let dma = self.iova_translator.translate_addr(
                b.iov_base as usize,
                b.iov_len,
            )?;

            let direction = if req_type.is_from_dev() {
                BufferDirection::DeviceToDriver
            } else {
                BufferDirection::DriverToDevice
            };

            segments.push(DmaSegment::new(
                dma.0,
                len,
                direction,
            ));
        }

        // The status byte is written by the device.
        let status_dma = self.iova_translator.translate_addr(
            status_ptr as usize,
            1,
        )?;

        segments.push(DmaSegment::new(
            status_dma.0,
            1,
            BufferDirection::DeviceToDriver,
        ));

        // The context must be registered before the request
        // can become visible to the backend.
        self.req_contexts[slot] = Some(context);

        let result = unsafe {
            self.vq.add(
                &segments,
                Token::new(slot),
                DescriptorLayout::Direct,
            )
        };

        if result.is_err() {
            // The request was not successfully submitted.
            self.req_contexts[slot].take();

            return Err(Error::new(
                ErrorKind::Other,
                "RFL VirtQueue add failed",
            ));
        }
        //END TEST

        Ok(())
    }

    fn queue_request(
        &mut self,
        req_type: VirtioBlkReqType,
        offset: u64,
        buf: &[iovec],
        context: C,
    ) -> Result<(), Error> {
        self.queue_request_full(req_type, offset, buf, None, context)
    }

    /// Reads from the disk image into a given iovec.
    ///
    /// `context` is an arbitrary caller-defined value that is returned in the corresponding
    /// [`Completion`] to allow associating the result with a specific request.
    ///
    /// # Safety
    ///
    /// The caller must ensure that the `iovec`/`iovcnt` pair is valid and all memory regions
    /// referenced by it are safe to access.
    pub unsafe fn readv(
        &mut self,
        offset: u64,
        iovec: *const iovec,
        iovcnt: usize,
        context: C,
    ) -> Result<(), Error> {
        let iov = unsafe { std::slice::from_raw_parts(iovec, iovcnt) };
        self.queue_request(VirtioBlkReqType::Read, offset, iov, context)
    }

    /// Reads from the disk image into a given buffer.
    ///
    /// `context` is an arbitrary caller-defined value that is returned in the corresponding
    /// [`Completion`] to allow associating the result with a specific request.
    ///
    /// # Safety
    ///
    /// The caller must ensure that the buffer described by `buf` and `len` is safe to access.
    pub unsafe fn read_raw(
        &mut self,
        offset: u64,
        buf: *mut u8,
        len: usize,
        context: C,
    ) -> Result<(), Error> {
        let iov = iovec {
            iov_base: buf as *mut c_void,
            iov_len: len,
        };

        self.queue_request(VirtioBlkReqType::Read, offset, &[iov], context)
    }

    /// Reads from the disk image into a given byte slice.
    ///
    /// `context` is an arbitrary caller-defined value that is returned in the corresponding
    /// [`Completion`] to allow associating the result with a specific request.
    pub fn read(&mut self, offset: u64, buf: &mut [u8], context: C) -> Result<(), Error> {
        unsafe { self.read_raw(offset, buf.as_mut_ptr(), buf.len(), context) }
    }

    /// Writes to the disk image from a given iovec.
    ///
    /// `context` is an arbitrary caller-defined value that is returned in the corresponding
    /// [`Completion`] to allow associating the result with a specific request.
    ///
    /// # Safety
    ///
    /// The caller must ensure that the `iovec`/`iovcnt` pair is valid and all memory regions
    /// referenced by it are safe to access.
    pub unsafe fn writev(
        &mut self,
        offset: u64,
        iovec: *const iovec,
        iovcnt: usize,
        context: C,
    ) -> Result<(), Error> {
        let iov = unsafe { std::slice::from_raw_parts(iovec, iovcnt) };
        self.queue_request(VirtioBlkReqType::Write, offset, iov, context)
    }

    /// Writes to the disk image from a given buffer.
    ///
    /// `context` is an arbitrary caller-defined value that is returned in the corresponding
    /// [`Completion`] to allow associating the result with a specific request.
    ///
    /// # Safety
    ///
    /// The caller must ensure that the buffer described by `buf` and `len` is safe to access.
    pub unsafe fn write_raw(
        &mut self,
        offset: u64,
        buf: *const u8,
        len: usize,
        context: C,
    ) -> Result<(), Error> {
        let iov = iovec {
            iov_base: buf as *mut c_void,
            iov_len: len,
        };

        self.queue_request(VirtioBlkReqType::Write, offset, &[iov], context)
    }

    /// Writes to the disk image from a given byte slice.
    ///
    /// `context` is an arbitrary caller-defined value that is returned in the corresponding
    /// [`Completion`] to allow associating the result with a specific request.
    pub fn write(&mut self, offset: u64, buf: &[u8], context: C) -> Result<(), Error> {
        unsafe { self.write_raw(offset, buf.as_ptr(), buf.len(), context) }
    }

    /// Discards an area in the disk image.
    ///
    /// After completion, the content of the specified area is undefined. Discard is only a hint
    /// and doing nothing is a valid implementation. This means that the discarded data may remain
    /// accessible, this is not a way to safely delete data.
    ///
    /// `context` is an arbitrary caller-defined value that is returned in the corresponding
    /// [`Completion`] to allow associating the result with a specific request.
    pub fn discard(&mut self, offset: u64, len: u64, context: C) -> Result<(), Error> {
        let dwz_data = DiscardWriteZeroesData::new(offset, len, false)?;
        self.queue_request_full(VirtioBlkReqType::Discard, 0, &[], Some(dwz_data), context)
    }

    /// Zeroes out an area in the disk image.
    ///
    /// If `unmap` is `true`, the area is tried to be deallocated if we know that it will read back
    /// as all zeroes afterwards. If it is `false`, allocated parts will remain allocated.
    ///
    /// `context` is an arbitrary caller-defined value that is returned in the corresponding
    /// [`Completion`] to allow associating the result with a specific request.
    pub fn write_zeroes(
        &mut self,
        offset: u64,
        len: u64,
        unmap: bool,
        context: C,
    ) -> Result<(), Error> {
        let dwz_data = DiscardWriteZeroesData::new(offset, len, unmap)?;
        self.queue_request_full(
            VirtioBlkReqType::WriteZeroes,
            0,
            &[],
            Some(dwz_data),
            context,
        )
    }

    /// Flushes the disk cache.
    ///
    /// This ensures that on successful completion, any requests that had completed before this
    /// flush request was issued are not sitting in any writeback cache, but are actually stored on
    /// disk.
    ///
    /// `context` is an arbitrary caller-defined value that is returned in the corresponding
    /// [`Completion`] to allow associating the result with a specific request.
    pub fn flush(&mut self, context: C) -> Result<(), Error> {
        self.queue_request(VirtioBlkReqType::Flush, 0, &[], context)
    }

    /// Returns the result for any completed requests.
    pub fn completions(&mut self) -> CompletionIter<'_, C> {
        // TEST
        CompletionIter {
            vq: &mut self.vq,
            req_bufs: self.req_bufs,
            req_contexts: self.req_contexts.as_mut(),
        }
        // END TEST
    }

    pub fn avail_notif_needed(&mut self) -> bool {
        // TEST
        self.vq.kick_prepare()
        // END TEST
    }

    pub fn set_used_notif_enabled(&mut self, enabled: bool) {
        // TEST
        if enabled {
            let _ = self.vq.enable_cb();
        } else {
            self.vq.disable_cb();
        }
        // END TEST
    }
}

// TEST
pub struct CompletionIter<'a, C> {
    vq: &'a mut VirtQueue<UserspaceHal>,
    req_bufs: NonNull<VirtioBlkReqBuf>,
    req_contexts: &'a mut [Option<C>],
}

impl<C> CompletionIter<'_, C> {
    pub fn has_next(&self) -> bool {
        self.vq.can_pop()
    }
}

impl<C> Iterator for CompletionIter<'_, C> {
    type Item = Completion<C>;

    fn next(&mut self) -> Option<Self::Item> {
        let (token, _len) = match self.vq.get_buf() {
            Ok(Some(completion)) => completion,
            Ok(None) => return None,
            Err(_) => {
                //TODO remove panic
                panic!("RFL VirtQueue completion error");
            }
        };

        let slot = token.value();

        let context = self.req_contexts
            .get_mut(slot)
            .unwrap_or_else(|| {
                panic!("RFL completion returned invalid token {slot}")
            })
            .take()
            .unwrap_or_else(|| {
                panic!("RFL completion returned inactive token {slot}")
            });

        let req_ptr = unsafe {
            self.req_bufs.as_ptr().add(slot)
        };

        // get_buf() has already applied the core's read ordering
        // before we inspect device-written request data.
        let status = unsafe {
            ptr::read_volatile(
                ptr::addr_of!((*req_ptr).status)
            )
        };

        Some(Completion {
            context,
            ret: match status {
                0 => 0,
                1 => -EIO,
                2 => -ENOTSUP,
                _ => -EPROTO,
            },
        })
    }
}

// END TEST