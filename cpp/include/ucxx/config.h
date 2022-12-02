/**
 * Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
 *
 * See file LICENSE for terms.
 */
#pragma once

#include <ucp/api/ucp.h>

#include <ucxx/typedefs.h>

namespace ucxx {

class Config {
 private:
  ucp_config_t* _handle{nullptr};
  ConfigMap _configMap;

  ucp_config_t* readUCXConfig(ConfigMap userOptions);

  ConfigMap ucxConfigToMap();

 public:
  Config()              = delete;
  Config(const Config&) = delete;
  Config& operator=(Config const&) = delete;
  Config(Config&& o)               = delete;
  Config& operator=(Config&& o) = delete;

  Config(ConfigMap userOptions);

  ~Config();

  ConfigMap get();

  ucp_config_t* getHandle();
};

}  // namespace ucxx
