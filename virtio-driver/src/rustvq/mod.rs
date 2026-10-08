#[allow(dead_code)]
pub(crate) mod userspace_hal;

#[allow(
    dead_code,
    clippy::doc_overindented_list_items,
    clippy::let_and_return,
    clippy::assign_op_pattern,
    clippy::incompatible_msrv,
)]
pub(crate) mod virtqueue;

pub(crate) fn new_virtqueue(
    hal: userspace_hal::UserspaceHal,
    queue_idx: u16,
    queue_size: u16,
    features: virtqueue::VirtQueueFeatures,
) -> Result<
    virtqueue::VirtQueue<userspace_hal::UserspaceHal>,
    virtqueue::Error<std::io::Error>,
> {
    virtqueue::VirtQueue::new(
        hal,
        queue_idx,
        queue_size,
        features,
    )
}
