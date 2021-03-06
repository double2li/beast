//
// Copyright (c) 2016-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

#ifndef BOOST_BEAST_WEBSOCKET_DETAIL_IMPL_PRNG_IPP
#define BOOST_BEAST_WEBSOCKET_DETAIL_IMPL_PRNG_IPP

#include <boost/beast/core/detail/chacha.hpp>
#include <boost/align/aligned_alloc.hpp>
#include <boost/throw_exception.hpp>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <new>
#include <random>
#include <stdexcept>

namespace boost {
namespace beast {
namespace websocket {
namespace detail {

//------------------------------------------------------------------------------

std::uint32_t const*
prng_seed(std::seed_seq* ss)
{
    struct data
    {
        std::uint32_t v[8];

        explicit
        data(std::seed_seq* pss)
        {
            if(! pss)
            {
                std::random_device g;
                std::seed_seq ss{
                    g(), g(), g(), g(),
                    g(), g(), g(), g()};
                ss.generate(v, v+8);
            }
            else
            {
                pss->generate(v, v+8);
            }
        }
    };
    static data const d(ss);
    return d.v;
}

//------------------------------------------------------------------------------

template<class T>
class prng_pool
{
    std::mutex m_;
    T* head_ = nullptr;

public:
    static
    prng_pool&
    instance()
    {
        static prng_pool p;
        return p;
    }

    ~prng_pool()
    {
        for(auto p = head_; p;)
        {
            auto next = p->next;
            p->~T();
            boost::alignment::aligned_free(p);
            p = next;
        }
    }

    prng::ref
    acquire()
    {
        {
            std::lock_guard<
                std::mutex> g(m_);
            if(head_)
            {
                auto p = head_;
                head_ = head_->next;
                return prng::ref(*p);
            }
        }
        auto p = boost::alignment::aligned_alloc(
            16, sizeof(T));
        if(! p)
            BOOST_THROW_EXCEPTION(std::bad_alloc{});
        return prng::ref(*(::new(p) T()));
    }

    void
    release(T& t)
    {
        std::lock_guard<
            std::mutex> g(m_);
        t.next = head_;
        head_ = &t;
    }
};

prng::ref
make_prng_no_tls(bool secure)
{
    class fast_prng final : public prng
    {
        int refs_ = 0;
        std::minstd_rand r_;

    public:
        fast_prng* next = nullptr;

        fast_prng()
            : r_([]
                {
                    static std::atomic<
                        std::uint64_t> nonce{0};
                    auto const pv = prng_seed();
                    return static_cast<value_type>(
                        pv[0] + pv[1] + pv[2] + pv[3] +
                        pv[4] + pv[5] + pv[6] + pv[7] +
                        ++nonce);
                }())
        {
        }

    protected:
        prng&
        acquire() noexcept override
        {
            ++refs_;
            return *this;
        }

        void
        release() noexcept override
        {
            if(--refs_ == 0)
                prng_pool<fast_prng>::instance().release(*this);
        }

        value_type
        operator()() noexcept override
        {
            return r_();
        }
    };

    class secure_prng final : public prng
    {
        int refs_ = 0;
        beast::detail::chacha<20> r_;

    public:
        secure_prng* next = nullptr;

        secure_prng()
            : r_(prng_seed(), []
                {
                    static std::atomic<
                        std::uint64_t> nonce{0};
                    return ++nonce;
                }())
        {
        }

    protected:
        prng&
        acquire() noexcept override
        {
            ++refs_;
            return *this;
        }

        void
        release() noexcept override
        {
            if(--refs_ == 0)
                prng_pool<secure_prng>::instance().release(*this);
        }

        value_type
        operator()() noexcept override
        {
            return r_();
        }
    };

    if(secure)
        return prng_pool<secure_prng>::instance().acquire();

    return prng_pool<fast_prng>::instance().acquire();
}

//------------------------------------------------------------------------------

#if ! BOOST_BEAST_NO_THREAD_LOCAL
prng::ref
make_prng_tls(bool secure)
{
    class fast_prng final : public prng
    {
        std::minstd_rand r_;

    public:
        fast_prng()
            : r_([]
                {
                    static std::atomic<
                        std::uint64_t> nonce{0};
                    auto const pv = prng_seed();
                    return static_cast<value_type>(
                        pv[0] + pv[1] + pv[2] + pv[3] +
                        pv[4] + pv[5] + pv[6] + pv[7] +
                        ++nonce);
                }())
        {
        }

    protected:
        prng&
        acquire() noexcept override
        {
            return *this;
        }

        void
        release() noexcept override
        {
        }

        value_type
        operator()() noexcept override
        {
            return r_();
        }
    };

    class secure_prng final : public prng
    {
        beast::detail::chacha<20> r_;

    public:
        secure_prng()
            : r_(prng_seed(), []
                {
                    static std::atomic<
                        std::uint64_t> nonce{0};
                    return ++nonce;
                }())
        {
        }

    protected:
        prng&
        acquire() noexcept override
        {
            return *this;
        }

        void
        release() noexcept override
        {
        }

        value_type
        operator()() noexcept override
        {
            return r_();
        }
    };

    if(secure)
    {
        thread_local secure_prng sp;
        return prng::ref(sp);
    }

    thread_local fast_prng fp;
    return prng::ref(fp);
}
#endif

//------------------------------------------------------------------------------

prng::ref
make_prng(bool secure)
{
#if BOOST_BEAST_NO_THREAD_LOCAL
    return make_prng_no_tls(secure);
#else
    return make_prng_tls(secure);
#endif
}

} // detail
} // websocket
} // beast
} // boost

#endif
