#pragma once
#include <inttypes.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <string>
#include <vector>
#include <sys/uio.h>
#include "../iovector.h"
#include "../utility.h"

namespace erpc
{
    struct buffer
    {
        void* _ptr = nullptr;
        size_t _len = 0;
        buffer() { }
        buffer(void* buf, size_t size) : _ptr(buf), _len(size) { }
        void* addr() const { return _ptr; }
        size_t size() const { return _len; }
        size_t length() const { return size(); }
        void assign(const void* ptr, size_t len) { _ptr = (void*)ptr; _len = len; }
    };

    // aligned buffer, which will be processed in the 1st pass of
    // serialization / deserialization process, helping to ensure alignment
    struct aligned_buffer : public buffer
    {
    };

    template<typename T>
    struct fixed_buffer : public buffer
    {
        using buffer::buffer;
        fixed_buffer(const T* x) : buffer(x, sizeof(*x)) { }
        void assign(const T* x) { buffer::assign(x, sizeof(*x)); }
        const T* get() const { return (T*)addr(); }
    };

    template<typename T>
    struct array : public buffer
    {
        array() { }
        array(const T* ptr, size_t size) { assign(ptr, size); }
        array(const std::vector<T>& vec) { assign(vec); }
        size_t size() const { return _len / sizeof(T); }
        const T* begin() const    { return (T*)_ptr; }
        const T* end() const      { return begin() + size(); }
        const T& operator[](long i) const { return ((char*)_ptr)[i]; }
        const T& front() const    { return (*this)[0]; }
        const T& back() const     { return (*this)[(long)size() - 1]; }
        void assign(const T* x, size_t size) { buffer::assign(x, sizeof(*x) * size); }
        void assign(const std::vector<T>& vec)
        {
            assign(&vec[0], vec.size());
        }

        struct iterator {
            T* p;
            iterator(T* t):p(t) {}
            const T& operator*() {
                return *p;
            }
            iterator& operator++(int) {
                p++;
                return *this;
            }
            bool operator!=(iterator rhs) {
                return p != rhs.p;
            }
            bool operator==(iterator rhs) {
                return p == rhs.p;
            }
        };
    };

    struct string : public array<char>
    {
        using base = array<char>;
        using base::base;

        template<size_t LEN>
        string(const char (&s)[LEN]) : base(s, LEN) { }
        string(const char* s) : base(s, strlen(s)+1) { }
        string(const std::string& s) { assign(s); }
        string() : base(nullptr, 0) { }
        const char* c_str() const { return begin(); }
        operator const char* () const { return c_str(); }
        void assign(const char* s) {
            base::assign(s, strlen(s) + 1);
        }
        void assign(const std::string& s)
        {
            array<char>::assign(s.c_str(), s.size()+1);
        }
    };

    struct iovec_array : public array<iovec>
    {
        // using base = array<iovec>;
        // using base::base;
        size_t summed_size;
        iovec_array() { }
        iovec_array(iovec* iov, int iovcnt)
        {
            assign(iov, iovcnt);
        }
        size_t assign(iovec* iov, int iovcnt)
        {
            assert(iovcnt >= 0);
            array<iovec>::assign(iov, (size_t)iovcnt);
            return sum();
        }
        size_t sum()
        {
            summed_size = 0;
            for (auto& v: *this)
                summed_size += v.iov_len;
            return summed_size;
        }
    };

    // Represents an iovec[] that has aligned iov_bases and iov_lens.
    // It will be processed in the 1st pass of serialization /
    // deserialization process, helping to ensure alignment.
    struct aligned_iovec_array : public iovec_array
    {
    };

    // structs of concrete messages MUST derive from `Message`,
    // and implement serialize_fields(), possbile with SERIALIZE_FIELDS()
    struct Message
    {
    public:
        template<typename AR>
        void serialize_fields(AR& ar)
        {
        }

    protected:
        template<typename AR>
        void reduce(AR& ar)
        {
        }
        template<typename AR, typename T, typename...Ts>
        void reduce(AR& ar, T& x, Ts&...xs)
        {
            ar.process_field(x);
            reduce(ar, xs...);
        }
    };

#define PROCESS_FIELDS(...)                     \
        template<typename AR>                   \
        void process_fields(AR& ar) {           \
            return reduce(ar, __VA_ARGS__);     \
        }



    template<typename Derived>  // The Curiously Recurring Template Pattern (CRTP)
    class ArchiveBase
    {
    public:
        Derived* d()
        {
            return static_cast<Derived*>(this);
        }

        template<typename T>
        void process_field(T& x)
        {
        }

        void process_field(buffer& x)
        {
            assert("must be re-implemented in derived classes");
        }

        template<typename T>
        void process_field(fixed_buffer<T>& x)
        {
            static_assert(
                !std::is_base_of<Message, T>::value,
                "no Messages are allowed");

            d()->process_field((buffer&)x);
            d()->process_field(*x.get());
        }

        template<typename T>
        void process_field(array<T>& x)
        {
            static_assert(
                !std::is_base_of<Message, T>::value,
                "no Messages are allowed");

            d()->process_field((buffer&)x);
            for (auto& i: x)
                d()->process_field(i);
        }

        void process_field(string& x)
        {
            d()->process_field((buffer&)x);
        }

        void process_field(iovec_array& x)
        {
            assert("must be re-implemented in derived classes");
        }

        // overload for embedded Message
        template<typename T, ENABLE_IF_BASE_OF(Message, T)>
        void process_field(T& x)
        {
            x.serialize_fields(*d());
        }
    };

    template<typename T>
    struct _FilterAlignedFields
    {
        T* _obj;
        bool _flag;
        void process_field(aligned_buffer& x)
        {
            if (_flag)
                _obj->process_field((buffer&)x);
        }
        void process_field(aligned_iovec_array& x)
        {
            if (_flag)
                _obj->process_field((iovec_array&)x);
        }
        template<typename P>
        void process_field(P& x)
        {
            if (!_flag)
                _obj->process_field(x);
        }
    };

    template<typename T>
    _FilterAlignedFields<T> FilterAlignedFields(T* obj, bool flag)
    {
        return _FilterAlignedFields<T>{obj, flag};
    }

    class SerializerIOV : public ArchiveBase<SerializerIOV>
    {
    public:
        IOVector iov;
        bool iovfull = false;

        using ArchiveBase<SerializerIOV>::process_field;

        void process_field(buffer& x)
        {
            if (iov.back_free_iovcnt() > 0) {
                if (x.size()>0)
                    iov.push_back(x.addr(), x.size());
            } else {
                iovfull = true;
            }
        }

        void process_field(iovec_array& x)
        {
            x.summed_size = 0;
            for (auto& v: x)
            {
                x.summed_size += v.iov_len;
                buffer buf(v.iov_base, v.iov_len);
                d()->process_field(buf);
            }
        }

        template<typename T>
        void serialize(T& x)
        {
            static_assert(
                std::is_base_of<Message, T>::value,
                "only Messages are permitted");

            // serialize aligned fields, non-aligned fields, and the main body
            auto aligned = FilterAlignedFields(this, true);
            x.process_fields(aligned);
            auto non_aligned = FilterAlignedFields(this, false);
            x.process_fields(non_aligned);
            buffer msg(&x, sizeof(x));
            d()->process_field(msg);
        }
    };

    class DeserializerIOV : public ArchiveBase<DeserializerIOV>
    {
    public:
        iovector* _iov;
        bool failed = false;

        using ArchiveBase<DeserializerIOV>::process_field;

        void process_field(buffer& x)
        {
            if (x.size()==0)
                return;
            x._ptr = _iov->extract_front_continuous(x.size());
            if (!x._ptr)
                failed = true;
        }

        void process_field(iovec_array& x)
        {
            iovector_view v;
            ssize_t ret = _iov->extract_front(x.summed_size, &v);
            if (ret == (ssize_t)x.summed_size) {
                x.assign(v.iov, v.iovcnt);
            } else {
                failed = true;
            }
        }

        template<typename T>
        T* deserialize(iovector* iov)
        {
            static_assert(
                std::is_base_of<Message, T>::value,
                "only Messages are permitted");

            // deserialize the main body from back
            _iov=iov;
            auto t = iov -> extract_back<T>();
            if (t) {
                // deserialize aligned fields, and non-aligned fields, from front
                auto aligned = FilterAlignedFields(this, true);
                t->process_fields(aligned);
                auto non_aligned = FilterAlignedFields(this, false);
                t->process_fields(non_aligned);
            } else {
                failed = true;
            }
            return t;
        }
    };
}

