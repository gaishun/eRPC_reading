#pragma once

#include "common.h"
#include "pkthdr.h"
#include "util/buffer.h"
#include "util/math_utils.h"
/**
 * TODO 首先看看这里面是怎么生成msgbuffer的
 *  * 通过RPC::alloc_msg_buffer_or_die(buf_size)申请一个buffer，根据这个size的大小来判断需要几个头，并预留空间。
 *  ? 但是头里面都有什么内容？不过好像也不用在乎这个。
 * TODO 看看这里面是怎么给这个buffer加上头的，比较大的buffer可能要多个头，是在哪里加的
 *  * 原来都是分配buffer，然后再填充信息，现在需要直接分配某个结构体的内容。大约知道怎么改了。
 * TODO 需要找到eRPC里面解析头的部分。
 *  * get_pkthdr_0和get_pkthdr_n两个函数，我们其实只需要一个头。
 * TODO 已经找到了erpc填充头的部分，后续把填充非零号头的部分注释掉就可以了，指填充一个头。
 * TODO 首先不需要很多个头，需要弄明白ali的头是怎么加上去的还有怎么解析的。！！！！重要🌟🌟🌟🌟🌟🌟🌟
 *    * 在rpc.cpp 的 do_send这个函数里面将头加上去的
 *    * 在rpc.cpp 的 do_recv_header这个函数里面解析头
 * TODO 然后，接口需要改一下，可以用反射的方式？或者使用protobuf约定的方式，用一个父类用来继承。
 *  ! 我们的buffer不需要多个头，只用一个头就行了，所以要把原来关于多个头的操作都删除掉
 * TODO 需要考虑，将原来发送的地方，改成我们的方式，就是用流的方式，发送过去。！！！！重要🌟🌟🌟🌟🌟🌟🌟是不是可以用复制粘贴进行？
 *  ? 继续研读论文关于这块的内容。
 */
namespace erpc {

// Forward declarations for friendship
class Session;

template <typename T>
class Rpc;

/**
 * @brief Applications store request and response messages in hugepage-backed
 * buffers called message buffers. These buffers are registered with the NIC,
 * allowing fast zero-copy transmission.
 *
 * A message buffer is allocated using Rpc::alloc_msg_buffer. Only its maximum
 * size is specified during allocation. Later, users can resize the message
 * buffer with Rpc::resize_msg_buffer to fit smaller requests or responses.
 * Rpc::free_msg_buffer frees a message buffer.
 *
 * A message buffer is invalid if its #buf pointer is null.
 */
class MsgBuffer {
  friend class CTransport;
  friend class Rpc<CTransport>;
  friend class Session;

 private:
  // * 快速获取第一个包的头
  /// Return a pointer to the pre-appended packet header of this MsgBuffer
  inline pkthdr_t *get_pkthdr_0() const {
    return reinterpret_cast<pkthdr_t *>(buf_ - sizeof(pkthdr_t));
  }
  
  // * 返回这个messagebuffer的第n个包的头的指针
  /// Return a pointer to the nth packet header of this MsgBuffer.
  /// get_pkthdr_0() is more efficient for retrieving the zeroth header.
  inline pkthdr_t *get_pkthdr_n(size_t n) const {
    if (unlikely(n == 0)) return get_pkthdr_0();
    return reinterpret_cast<pkthdr_t *>(
        buf_ + round_up<sizeof(size_t)>(max_data_size_) +
        (n - 1) * sizeof(pkthdr_t));
  }
  
  //* 获取这个包的信息
  std::string get_pkthdr_str(size_t pkt_idx) const {
    return get_pkthdr_n(pkt_idx)->to_string();
  }

  // * 是一个动态分配的buffer还是一个假的buffer？
  /// Return true iff this MsgBuffer uses a dynamically-allocated MsgBuffer.
  /// This function does not sanity-check other fields.
  inline bool is_dynamic() const { return buffer_.buf_ != nullptr; }
  // * 检查这个buffer是不是不同了
  /// Check if this MsgBuffer is buried
  inline bool is_buried() const {
    return (buf_ == nullptr && buffer_.buf_ == nullptr);
  }

  //* 获取这个pkt的大小
  /// Get the packet size (i.e., including packet header) of a packet
  template <size_t kMaxDataPerPkt>
  inline size_t get_pkt_size(size_t pkt_idx) const {
    size_t offset = pkt_idx * kMaxDataPerPkt;
    return sizeof(pkthdr_t) + (std::min)(kMaxDataPerPkt, data_size_ - offset);
  }
  // * 用一个string表示这个MsgBuffer.
  /// Return a string representation of this MsgBuffer
  std::string to_string() const {
    if (buf_ == nullptr) return "[Invalid]";

    std::ostringstream ret;
    ret << "[buf " << static_cast<void *>(buf_) << ", "
        << "buffer " << buffer_.to_string() << ", "
        << "data_size " << data_size_ << "(" << max_data_size_ << "), "
        << "pkts " << num_pkts_ << "(" << max_num_pkts_ << ")]";
    return ret.str();
  }
  //? 动态构造一个MessageBuffer.
  /// Construct a MsgBuffer with a dynamic Buffer allocated by eRPC.
  /// The zeroth packet header is stored at \p buffer.buf. \p buffer must have
  /// space for at least \p max_data_bytes, and \p max_num_pkts packet headers.
  // ! 只有一个buf_但是我可能有n个包。
  // * 需要几个包根据配置获得，在huge_page.alloc函数中得到需要几个包，如果需要多个包的话，还是比较麻烦的。
  MsgBuffer(Buffer buffer, size_t max_data_size, size_t max_num_pkts)
      : buffer_(buffer), // * 指向第零个包的头，而不是数据位置
        max_data_size_(max_data_size),  // * 最大可以支持数据去的大小
        data_size_(max_data_size),  // * 数据区的大小 _buf 大小
        max_num_pkts_(max_num_pkts), // * 最多可以支持几个包 
        num_pkts_(max_num_pkts),  // * 有几个包
        buf_(buffer.buf_ + sizeof(pkthdr_t)) // * 指向数据位置
        {
    // * 确定这个buffer 不是空指针
    assert(buffer.buf_ != nullptr);  // buffer must be valid
    // data_size can be 0
    // * 至少要有一个包吧，如果包的数量是0的话，那就不行了。
    assert(max_num_pkts >= 1);
    // * 判断这个buffer的大小跟分配的空间'max_data_size+max_num_pkts*sizeof(pkthdr_t)'是不是够大。
    assert(buffer.class_size_ >=
           max_data_size + max_num_pkts * sizeof(pkthdr_t));
    // * 拿到第一个头的指针
    pkthdr_t *pkthdr_0 = get_pkthdr_0();
    pkthdr_0->magic_ = kPktHdrMagic;

    // UDP checksum for raw Ethernet. Useless for other transports.
    static_assert(sizeof(pkthdr_t::headroom_) == kHeadroom + 2, "");
    pkthdr_0->headroom_[kHeadroom] = 0;
    pkthdr_0->headroom_[kHeadroom + 1] = 0;
  }



  //* 用已经接受的包构建一个假的packet？
  /// Construct a single-packet "fake" MsgBuffer using a received packet,
  /// setting \p buffer to invalid so that we know not to free it.
  /// \p pkt must have space for \p max_data_bytes and one packet header.
  MsgBuffer(pkthdr_t *pkthdr, size_t max_data_size)
      : max_data_size_(max_data_size),
        data_size_(max_data_size),
        max_num_pkts_(1),
        num_pkts_(1),
        buf_(reinterpret_cast<uint8_t *>(pkthdr) + sizeof(pkthdr_t)) {
    // max_data_size can be zero for control packets, so can't assert

    buffer_.buf_ = nullptr;  // Mark as a non-dynamic ("fake") MsgBuffer
  }
  // * 重新设置大小
  /// Resize this MsgBuffer to any size smaller than its maximum allocation
  inline void resize(size_t new_data_size, size_t new_num_pkts) {
    assert(new_data_size <= max_data_size_);
    assert(new_num_pkts <= max_num_pkts_);
    data_size_ = new_data_size;
    num_pkts_ = new_num_pkts;
  }

 public:
  // The real constructors are private
  MsgBuffer() {}
  ~MsgBuffer() {}

  /**
   * Return the current amount of data in this message buffer. This can be
   * smaller than it's maximum data capacity due to resizing.
   */
  inline size_t get_data_size() const { return data_size_; }

 private:
  /// The optional backing hugepage buffer. buffer.buf points to the zeroth
  /// packet header, i.e., not application data.
  Buffer buffer_;

  // Size info
  size_t max_data_size_;  ///< Max data bytes in the MsgBuffer
  size_t data_size_;      ///< Current data bytes in the MsgBuffer
  size_t max_num_pkts_;   ///< Max number of packets in this MsgBuffer
  size_t num_pkts_;       ///< Current number of packets in this MsgBuffer

 public:
  // * 这个buf_指向这个buffer数据段的第一个字节的位置。
  /// Pointer to the first application data byte. The message buffer is invalid
  /// invalid if this is null.
  uint8_t *buf_;
};
}  // namespace erpc
