/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#ifndef ONEFLOW_CORE_CONTROL_CTRL_BOOTSTRAP_H_
#define ONEFLOW_CORE_CONTROL_CTRL_BOOTSTRAP_H_

#include "oneflow/core/control/ctrl_bootstrap.pb.h"
#include "oneflow/core/job/env_desc.h"
#include "oneflow/core/common/maybe.h"

namespace oneflow {

class ProcessCtx;

class CtrlBootstrap {
 public:
  virtual ~CtrlBootstrap() {}

  virtual Maybe<void> InitProcessCtx(int64_t port, ProcessCtx* process_ctx) = 0;

 protected:
  virtual int64_t rank() const = 0;
  virtual int64_t world_size() const = 0;
  virtual std::string host() const = 0;

  CtrlBootstrap() = default;
};

class HostListBootstrapServer;
class HostListBootstrapClient;

class HostListCtrlBootstrap final : public CtrlBootstrap {
 public:
  explicit HostListCtrlBootstrap(const EnvDesc& env_desc);
  ~HostListCtrlBootstrap() override;

  Maybe<void> InitProcessCtx(int64_t port, ProcessCtx* process_ctx) override;

 private:
  std::string host() const override { return host_; }
  int64_t rank() const override { return rank_; }
  int64_t world_size() const override { return world_size_; }

  // Uses shared_ptr and forward declaration to avoid `#include ...`
  std::shared_ptr<HostListBootstrapServer> bootstrap_server_;
  std::shared_ptr<HostListBootstrapClient> bootstrap_client_;

  std::string host_;
  int64_t rank_;
  int64_t world_size_;
};

}  // namespace oneflow

#endif  // ONEFLOW_CORE_CONTROL_CTRL_BOOTSTRAP_H_