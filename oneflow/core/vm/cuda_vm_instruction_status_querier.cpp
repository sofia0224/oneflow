#include "oneflow/core/vm/cuda_vm_instruction_status_querier.h"
#include "oneflow/core/device/device_context.h"

namespace oneflow {

bool CudaVmInstrStatusQuerier::event_completed() const {
  CudaCurrentDeviceGuard guard(device_id_);
  return cudaEventQuery(event_) == cudaSuccess;
}

void CudaVmInstrStatusQuerier::SetLaunched(DeviceCtx* device_ctx) {
  CudaCurrentDeviceGuard guard(device_id_);
  CudaCheck(cudaEventCreateWithFlags(&event_, cudaEventBlockingSync | cudaEventDisableTiming));
  CudaCheck(cudaEventRecord(event_, device_ctx->cuda_stream()));
  launched_ = true;
}

}  // namespace oneflow
