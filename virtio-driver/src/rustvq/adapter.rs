use crate::transports::VirtqueueConfig;

use super::{
    userspace_hal::UserspaceHal,
    virtqueue::{VirtQueue, VirtQueueError},
};

pub(crate) fn transport_config(
    vq: &VirtQueue<UserspaceHal>,
) -> Result<VirtqueueConfig, VirtQueueError> {
    Ok(VirtqueueConfig {
        queue_size: vq.size(),
        desc_device_addr: vq.descriptor_dma_addr()?,
        driver_device_addr: vq.driver_area_dma_addr()?,
        device_device_addr: vq.device_area_dma_addr()?,
    })
}