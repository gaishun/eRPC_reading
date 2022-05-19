#include "common.h"
erpc::Rpc<erpc::CTransport> *rpc;
erpc::MsgBuffer req;
erpc::MsgBuffer resp;

void cont_func(void *, void *) { printf("%s\n", resp.buf_); }

void sm_handler(int, erpc::SmEventType, erpc::SmErrType, void *) {}

int main() {
  std::string client_uri = kClientHostname + ":" + std::to_string(kUDPPort);
  erpc::Nexus nexus(client_uri);

  rpc = new erpc::Rpc<erpc::CTransport>(&nexus, nullptr, 0, sm_handler);

  std::string server_uri = kServerHostname + ":" + std::to_string(kUDPPort);
  // * 这里客户端给server发送了一个udp包请求链接
  int session_num = rpc->create_session(server_uri, 0);
  // * 这里状态应该是 inprocess。 然后所以一直通过 run_event_loop_once 检测状态，直到连上了为止
  while (!rpc->is_connected(session_num)) rpc->run_event_loop_once();

  req = rpc->alloc_msg_buffer_or_die(kMsgSize);
  req.get_data_size();
  
  
  resp = rpc->alloc_msg_buffer_or_die(kMsgSize);

  rpc->enqueue_request(session_num, kReqType, &req, &resp, cont_func,ga nullptr);
  rpc->run_event_loop(100);

  delete rpc;
}
