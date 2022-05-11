/**
 * Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
 *
 * See file LICENSE for terms.
 */
#pragma once

#include <netdb.h>

#include <memory>
#include <string>

#include <ucp/api/ucp.h>

#include <ucxx/address.h>
#include <ucxx/component.h>
#include <ucxx/exception.h>
#include <ucxx/listener.h>
#include <ucxx/sockaddr_utils.h>
#include <ucxx/transfer_stream.h>
#include <ucxx/transfer_tag.h>
#include <ucxx/typedefs.h>
#include <ucxx/utils.h>
#include <ucxx/worker.h>

namespace ucxx {

class UCXXRequest;

struct EpParamsDeleter {
  void operator()(ucp_ep_params_t* ptr)
  {
    if (ptr != nullptr && ptr->field_mask & UCP_EP_PARAM_FIELD_SOCK_ADDR)
      sockaddr_utils_free(&ptr->sockaddr);
  }
};

typedef struct error_callback_data {
  ucs_status_t status;
  inflight_requests_t inflightRequests;
  std::function<void(void*)> closeCallback;
  void* closeCallbackArg;
  std::shared_ptr<UCXXWorker> worker;
} error_callback_data_t;

void _err_cb(void* arg, ucp_ep_h ep, ucs_status_t status)
{
  error_callback_data_t* data = (error_callback_data_t*)arg;
  data->status                = status;
  data->worker->scheduleRequestCancel(data->inflightRequests);
  if (data->closeCallback) {
    data->closeCallback(data->closeCallbackArg);
    data->closeCallback    = nullptr;
    data->closeCallbackArg = nullptr;
  }

  // Connection reset and timeout often represent just a normal remote
  // endpoint disconnect
  if (status == UCS_ERR_CONNECTION_RESET || status == UCS_ERR_ENDPOINT_TIMEOUT)
    ucxx_debug("Error callback for endpoint %p called with status %d: %s",
               ep,
               status,
               ucs_status_string(status));
  else
    ucxx_error("Error callback for endpoint %p called with status %d: %s",
               ep,
               status,
               ucs_status_string(status));
}

class UCXXEndpoint : public UCXXComponent {
 private:
  ucp_ep_h _handle{nullptr};
  bool _endpoint_error_handling{true};
  std::unique_ptr<error_callback_data_t> _callbackData{nullptr};
  inflight_requests_t _inflightRequests{std::make_shared<inflight_request_map_t>()};

  UCXXEndpoint(std::shared_ptr<UCXXComponent> worker_or_listener,
               std::unique_ptr<ucp_ep_params_t, EpParamsDeleter> params,
               bool endpoint_error_handling)
    : _endpoint_error_handling{endpoint_error_handling}
  {
    auto worker = UCXXEndpoint::getWorker(worker_or_listener);

    if (worker == nullptr || worker->get_handle() == nullptr)
      throw ucxx::UCXXError("Worker not initialized");

    setParent(worker_or_listener);

    _callbackData = std::make_unique<error_callback_data_t>((error_callback_data_t){
      .status = UCS_OK, .inflightRequests = _inflightRequests, .worker = worker});

    params->err_mode =
      (endpoint_error_handling ? UCP_ERR_HANDLING_MODE_PEER : UCP_ERR_HANDLING_MODE_NONE);
    params->err_handler.cb  = _err_cb;
    params->err_handler.arg = _callbackData.get();

    assert_ucs_status(ucp_ep_create(worker->get_handle(), params.get(), &_handle));
  }

 public:
  UCXXEndpoint()                    = delete;
  UCXXEndpoint(const UCXXEndpoint&) = delete;
  UCXXEndpoint& operator=(UCXXEndpoint const&) = delete;
  UCXXEndpoint(UCXXEndpoint&& o)               = delete;
  UCXXEndpoint& operator=(UCXXEndpoint&& o) = delete;

  ~UCXXEndpoint()
  {
    if (_handle == nullptr) return;

    // Close the endpoint
    unsigned close_mode = UCP_EP_CLOSE_MODE_FORCE;
    if (_endpoint_error_handling and _callbackData->status != UCS_OK) {
      // We force close endpoint if endpoint error handling is enabled and
      // the endpoint status is not UCS_OK
      close_mode = UCP_EP_CLOSE_MODE_FORCE;
    }
    ucs_status_ptr_t status = ucp_ep_close_nb(_handle, close_mode);
    if (UCS_PTR_IS_PTR(status)) {
      auto worker = UCXXEndpoint::getWorker(_parent);
      while (ucp_request_check_status(status) == UCS_INPROGRESS)
        worker->progress();
      ucp_request_free(status);
    } else if (UCS_PTR_STATUS(status) != UCS_OK) {
      ucxx_error("Error while closing endpoint: %s", ucs_status_string(UCS_PTR_STATUS(status)));
    }
  }

  static std::shared_ptr<UCXXWorker> getWorker(std::shared_ptr<UCXXComponent> worker_or_listener)
  {
    auto worker = std::dynamic_pointer_cast<UCXXWorker>(worker_or_listener);
    if (worker == nullptr) {
      auto listener = std::dynamic_pointer_cast<UCXXListener>(worker_or_listener);
      worker        = std::dynamic_pointer_cast<UCXXWorker>(listener->getParent());
    }
    return worker;
  }

  friend std::shared_ptr<UCXXEndpoint> createEndpointFromHostname(
    std::shared_ptr<UCXXWorker> worker,
    std::string ip_address,
    uint16_t port,
    bool endpoint_error_handling)
  {
    if (worker == nullptr || worker->get_handle() == nullptr)
      throw ucxx::UCXXError("Worker not initialized");

    auto params = std::unique_ptr<ucp_ep_params_t, EpParamsDeleter>(new ucp_ep_params_t);

    struct hostent* hostname = gethostbyname(ip_address.c_str());
    if (hostname == nullptr) throw ucxx::UCXXError(std::string("Invalid IP address or hostname"));

    params->field_mask = UCP_EP_PARAM_FIELD_FLAGS | UCP_EP_PARAM_FIELD_SOCK_ADDR |
                         UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE | UCP_EP_PARAM_FIELD_ERR_HANDLER;
    params->flags = UCP_EP_PARAMS_FLAGS_CLIENT_SERVER;
    if (sockaddr_utils_set(&params->sockaddr, hostname->h_name, port)) throw std::bad_alloc();

    return std::shared_ptr<UCXXEndpoint>(
      new UCXXEndpoint(worker, std::move(params), endpoint_error_handling));
  }

  friend std::shared_ptr<UCXXEndpoint> createEndpointFromConnRequest(
    std::shared_ptr<UCXXListener> listener,
    ucp_conn_request_h conn_request,
    bool endpoint_error_handling)
  {
    if (listener == nullptr || listener->get_handle() == nullptr)
      throw ucxx::UCXXError("Worker not initialized");

    auto params        = std::unique_ptr<ucp_ep_params_t, EpParamsDeleter>(new ucp_ep_params_t);
    params->field_mask = UCP_EP_PARAM_FIELD_FLAGS | UCP_EP_PARAM_FIELD_CONN_REQUEST |
                         UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE | UCP_EP_PARAM_FIELD_ERR_HANDLER;
    params->flags        = UCP_EP_PARAMS_FLAGS_NO_LOOPBACK;
    params->conn_request = conn_request;

    return std::shared_ptr<UCXXEndpoint>(
      new UCXXEndpoint(listener, std::move(params), endpoint_error_handling));
  }

  friend std::shared_ptr<UCXXEndpoint> createEndpointFromWorkerAddress(
    std::shared_ptr<UCXXWorker> worker,
    std::shared_ptr<UCXXAddress> address,
    bool endpoint_error_handling)
  {
    if (worker == nullptr || worker->get_handle() == nullptr)
      throw ucxx::UCXXError("Worker not initialized");
    if (address == nullptr || address->getHandle() == nullptr || address->getLength() == 0)
      throw ucxx::UCXXError("Address not initialized");

    auto params        = std::unique_ptr<ucp_ep_params_t, EpParamsDeleter>(new ucp_ep_params_t);
    params->field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS | UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
                         UCP_EP_PARAM_FIELD_ERR_HANDLER;
    params->address = address->getHandle();

    return std::shared_ptr<UCXXEndpoint>(
      new UCXXEndpoint(worker, std::move(params), endpoint_error_handling));
  }

  ucp_ep_h getHandle() { return _handle; }

  bool isAlive() const
  {
    if (!_endpoint_error_handling) return true;

    return _callbackData->status == UCS_OK;
  }

  void raiseOnError()
  {
    ucs_status_t status = _callbackData->status;

    if (status == UCS_OK || !_endpoint_error_handling) return;

    std::string statusString{ucs_status_string(status)};
    std::stringstream errorMsgStream;
    errorMsgStream << "Endpoint " << std::hex << _handle << " error: " << statusString;

    if (status == UCS_ERR_CONNECTION_RESET)
      throw UCXXConnectionResetError(errorMsgStream.str());
    else
      throw UCXXError(errorMsgStream.str());
  }

  void setCloseCallback(std::function<void(void*)> closeCallback, void* closeCallbackArg)
  {
    _callbackData->closeCallback    = closeCallback;
    _callbackData->closeCallbackArg = closeCallbackArg;
  }

  std::shared_ptr<UCXXRequest> createRequest(std::shared_ptr<ucxx_request_t> request)
  {
    auto endpoint = std::dynamic_pointer_cast<UCXXEndpoint>(shared_from_this());
    auto req      = ucxx::createRequest(endpoint, _inflightRequests, request);
    std::weak_ptr<UCXXRequest> weak_req = req;
    _inflightRequests->insert({req.get(), weak_req});
    return req;
  }

  std::shared_ptr<UCXXRequest> stream_send(void* buffer, size_t length)
  {
    auto worker  = UCXXEndpoint::getWorker(_parent);
    auto request = stream_msg(worker, _handle, true, buffer, length);
    return createRequest(request);
  }

  std::shared_ptr<UCXXRequest> stream_recv(void* buffer, size_t length)
  {
    auto worker  = UCXXEndpoint::getWorker(_parent);
    auto request = stream_msg(worker, _handle, false, buffer, length);
    return createRequest(request);
  }

  std::shared_ptr<UCXXRequest> tag_send(void* buffer,
                                        size_t length,
                                        ucp_tag_t tag,
                                        void* callbackFunction             = nullptr,
                                        std::shared_ptr<void> callbackData = nullptr)
  {
    auto worker = UCXXEndpoint::getWorker(_parent);
    auto request =
      tag_msg(worker, _handle, true, buffer, length, tag, callbackFunction, callbackData);
    return createRequest(request);
  }

  std::shared_ptr<UCXXRequest> tag_recv(void* buffer,
                                        size_t length,
                                        ucp_tag_t tag,
                                        void* callbackFunction             = nullptr,
                                        std::shared_ptr<void> callbackData = nullptr)
  {
    auto worker = UCXXEndpoint::getWorker(_parent);
    auto request =
      tag_msg(worker, _handle, false, buffer, length, tag, callbackFunction, callbackData);
    return createRequest(request);
  }
};

}  // namespace ucxx
