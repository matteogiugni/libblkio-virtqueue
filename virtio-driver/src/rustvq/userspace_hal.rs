use std::{
    alloc::{alloc, dealloc, Layout},
    cell::Cell,
    io::{Error, ErrorKind},
    ptr::NonNull,
    sync::atomic::{fence, Ordering},
};

use super::virtqueue::hal::{
    Allocator,
    DmaRegion,
    Hal,
    MemoryRegion,
};

pub(crate) struct UserspaceHal {
    cpu_base: NonNull<u8>,
    dma_base: u64,
    size: usize,
    //CHECK cell is not sync
    next_offset: Cell<usize>,
}

impl UserspaceHal {
    /// Creates an instance over an already device-visible memory region.
    /// `dma_base` is the device-visible address corresponding to `cpu_base`.
    ///
    /// # Safety
    ///
    /// The caller must guarantee that the backing region remains valid
    /// for the entire lifetime of every DMA allocation returned by this instance.
    pub(crate) unsafe fn new(
        cpu_base: NonNull<u8>,
        dma_base: u64,
        size: usize,
    ) -> Self {
        Self {
            cpu_base,
            dma_base,
            size,
            next_offset: Cell::new(0),
        }
    }

    fn align_up(
        value: usize,
        align: usize,
    ) -> Result<usize, Error> {
        if align == 0 || !align.is_power_of_two() {
            return Err(Error::new(
                ErrorKind::InvalidInput,
                "invalid alignment",
            ));
        }

        value
            .checked_add(align - 1)
            .map(|v| v & !(align - 1))
            .ok_or_else(|| {
                Error::new(
                    ErrorKind::InvalidInput,
                    "allocation offset overflow",
                )
            })
    }
}

unsafe impl Allocator for UserspaceHal {
    type Error = Error;

    fn alloc(
        size: usize,
        align: usize,
    ) -> Result<MemoryRegion<Self>, Self::Error> {
        if size == 0 || align == 0 || !align.is_power_of_two() {
            return Err(Error::new(
                ErrorKind::InvalidInput,
                "invalid allocation layout",
            ));
        }

        let layout = Layout::from_size_align(size, align)
            .map_err(|_| {
                Error::new(
                    ErrorKind::InvalidInput,
                    "invalid allocation layout",
                )
            })?;

        let ptr = unsafe { alloc(layout) };

        let cpu_addr = NonNull::new(ptr).ok_or_else(|| {
            Error::new(
                ErrorKind::OutOfMemory,
                "userspace allocation failed",
            )
        })?;

        Ok(unsafe {
            MemoryRegion::new(
                cpu_addr,
                size,
                align,
            )
        })
    }

    unsafe fn dealloc(
        ptr: NonNull<u8>,
        size: usize,
        align: usize,
    ) {
        let layout =
            unsafe { Layout::from_size_align_unchecked(size, align) };

        unsafe {
            dealloc(ptr.as_ptr(), layout);
        }
    }

    fn dma_alloc(
        &self,
        size: usize,
        align: usize,
    ) -> Result<DmaRegion, Self::Error> {
        if size == 0 {
            return Err(Error::new(
                ErrorKind::InvalidInput,
                "zero-sized DMA allocation",
            ));
        }

        let current = self.next_offset.get();
        let offset = Self::align_up(current, align)?;

        let end = offset.checked_add(size).ok_or_else(|| {
            Error::new(
                ErrorKind::InvalidInput,
                "DMA allocation size overflow",
            )
        })?;

        if end > self.size {
            return Err(Error::new(
                ErrorKind::OutOfMemory,
                "DMA arena exhausted",
            ));
        }

        let cpu_addr = unsafe {
            NonNull::new_unchecked(
                self.cpu_base.as_ptr().add(offset),
            )
        };

        let dma_addr = self
            .dma_base
            .checked_add(offset as u64)
            .ok_or_else(|| {
                Error::new(
                    ErrorKind::InvalidInput,
                    "DMA address overflow",
                )
            })?;

        unsafe {
            std::ptr::write_bytes(
                cpu_addr.as_ptr(),
                0,
                size,
            );
        }

        self.next_offset.set(end);

        Ok(DmaRegion {
            cpu_addr,
            dma_addr,
            size,
        })
    }

    unsafe fn dma_free(
        &self,
        _region: DmaRegion,
    ) {
        // Intentionally a no-op for the first direct-descriptor test.
        //
        // The entire backing arena lives longer than the VirtQueue and
        // is released as a whole during queue teardown.
    }
}

unsafe impl Hal for UserspaceHal {
    //TODO fence is not enough for device memory barriers
    #[inline]
    fn write_barrier(&self) {
        fence(Ordering::Release);
    }

    #[inline]
    fn read_barrier(&self) {
        fence(Ordering::Acquire);
    }

    #[inline]
    fn mb(&self) {
        fence(Ordering::SeqCst);
    }
}