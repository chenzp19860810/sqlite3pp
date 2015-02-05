// sqlite3ppext.h
//
// The MIT License
//
// Copyright (c) 2015 Wongoo Lee (iwongu at gmail dot com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef SQLITE3PPEXT_H
#define SQLITE3PPEXT_H

#include <cstddef>
#include <map>
#include <tuple>
#include <type_traits>
#include <utility>

#include "sqlite3pp.h"

namespace sqlite3pp
{

  namespace ext
  {
    template<class F>
    struct function_traits;

    template<class R, class... Args>
    struct function_traits<R(*)(Args...)> : public function_traits<R(Args...)>
    {};

    template<class R, class... Args>
    struct function_traits<R(Args...)>
    {
      using return_type = R;

      static constexpr std::size_t arity = sizeof...(Args);

      template <std::size_t N>
      struct argument
      {
        static_assert(N < arity, "error: invalid parameter index.");
        using type = typename std::tuple_element<N, std::tuple<Args...>>::type;
      };
    };

    class context : noncopyable
    {
      friend function;

    public:
      explicit context(sqlite3_context* ctx, int nargs = 0, sqlite3_value** values = nullptr);

      int args_count() const;
      int args_bytes(int idx) const;
      int args_type(int idx) const;

      template <class T> T get(int idx) const {
        return get(idx, T());
      }

      void result(int value);
      void result(double value);
      void result(long long int value);
      void result(std::string const& value);
      void result(char const* value, bool fstatic = true);
      void result(void const* value, int n, bool fstatic = true);
      void result();
      void result(null_type);
      void result_copy(int idx);
      void result_error(char const* msg);

      void* aggregate_data(int size);
      int aggregate_count();

     private:
      int get(int idx, int) const;
      double get(int idx, double) const;
      long long int get(int idx, long long int) const;
      char const* get(int idx, char const*) const;
      std::string get(int idx, std::string) const;
      void const* get(int idx, void const*) const;

      template <class... Ts>
      std::tuple<Ts...> to_tuple() {
        return to_tuple_impl(0, *this, std::tuple<Ts...>());
      }
      template<class H, class... Ts>
      static inline std::tuple<H, Ts...> to_tuple_impl(int index, const context& c, std::tuple<H, Ts...>&&)
      {
        auto h = std::make_tuple(c.context::get<H>(index));
        return std::tuple_cat(h, to_tuple_impl(++index, c, std::tuple<Ts...>()));
      }
      static inline std::tuple<> to_tuple_impl(int index, const context& c, std::tuple<>&&)
      {
        return std::tuple<>();
      }

     private:
      sqlite3_context* ctx_;
      int nargs_;
      sqlite3_value** values_;
    };

    namespace
    {
      template<size_t N>
      struct Apply {
        template<typename F, typename T, typename... A>
        static inline auto apply(F&& f, T&& t, A&&... a)
          -> decltype(Apply<N-1>::apply(std::forward<F>(f),
                                        std::forward<T>(t),
                                        std::get<N-1>(std::forward<T>(t)),
                                        std::forward<A>(a)...))
        {
          return Apply<N-1>::apply(std::forward<F>(f),
                                   std::forward<T>(t),
                                   std::get<N-1>(std::forward<T>(t)),
                                   std::forward<A>(a)...);
        }
      };

      template<>
      struct Apply<0> {
        template<typename F, typename T, typename... A>
        static inline auto apply(F&& f, T&&, A&&... a)
          -> decltype(std::forward<F>(f)(std::forward<A>(a)...))
        {
          return std::forward<F>(f)(std::forward<A>(a)...);
        }
      };

      template<typename F, typename T>
      inline auto apply(F&& f, T&& t)
        -> decltype(Apply<std::tuple_size<typename std::decay<T>::type>::value>::apply(std::forward<F>(f), std::forward<T>(t)))
      {
        return Apply<std::tuple_size<typename std::decay<T>::type>::value>::apply(
            std::forward<F>(f), std::forward<T>(t));
      }

      template <class R, class... Ps>
      void functionx_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values)
      {
        context c(ctx, nargs, values);
        auto f = static_cast<std::function<R (Ps...)>*>(sqlite3_user_data(ctx));
        c.result(apply(*f, c.to_tuple<Ps...>()));
      }
    }


    class function : noncopyable
    {
     public:
      using function_handler = std::function<void (context&)>;
      using pfunction_base = std::shared_ptr<void>;

      explicit function(database& db);

      int create(char const* name, function_handler h, int nargs = 0);

      template <class F> int create(char const* name, std::function<F> h) {
        fh_[name] = std::shared_ptr<void>(new std::function<F>(h));
        return create_function_impl<function_traits<F>::arity, F>()(db_, fh_[name].get(), name);
      }

     private:

      template <int N, class F>
      struct create_function_impl;

      template <class F>
      struct create_function_impl<0, F> {
        int operator()(sqlite3* db, void* fh, char const* name) {
          using FT = function_traits<F>;
          using R = typename FT::return_type;

          return sqlite3_create_function(db, name, 0, SQLITE_UTF8, fh,
                                         functionx_impl<R>,
                                         0, 0);
        }
      };

      template <class F>
      struct create_function_impl<1, F> {
        int operator()(sqlite3* db, void* fh, char const* name) {
          using FT = function_traits<F>;
          using R = typename FT::return_type;
          using P1 = typename FT::template argument<0>::type;

          return sqlite3_create_function(db, name, 1, SQLITE_UTF8, fh,
                                         functionx_impl<R, P1>,
                                         0, 0);
        }
      };

      template <class F>
      struct create_function_impl<2, F> {
        int operator()(sqlite3* db, void* fh, char const* name) {
          using FT = function_traits<F>;
          using R = typename FT::return_type;
          using P1 = typename FT::template argument<0>::type;
          using P2 = typename FT::template argument<1>::type;

          return sqlite3_create_function(db, name, 2, SQLITE_UTF8, fh,
                                         functionx_impl<R, P1, P2>,
                                         0, 0);
        }
      };

      template <class F>
      struct create_function_impl<3, F> {
        int operator()(sqlite3* db, void* fh, char const* name) {
          using FT = function_traits<F>;
          using R = typename FT::return_type;
          using P1 = typename FT::template argument<0>::type;
          using P2 = typename FT::template argument<1>::type;
          using P3 = typename FT::template argument<2>::type;

          return sqlite3_create_function(db, name, 3, SQLITE_UTF8, fh,
                                         functionx_impl<R, P1, P2, P3>,
                                         0, 0);
        }
      };

      template <class F>
      struct create_function_impl<4, F> {
        int operator()(sqlite3* db, void* fh, char const* name) {
          using FT = function_traits<F>;
          using R = typename FT::return_type;
          using P1 = typename FT::template argument<0>::type;
          using P2 = typename FT::template argument<1>::type;
          using P3 = typename FT::template argument<2>::type;
          using P4 = typename FT::template argument<3>::type;

          return sqlite3_create_function(db, name, 4, SQLITE_UTF8, fh,
                                         functionx_impl<R, P1, P2, P3, P4>,
                                         0, 0);
        }
      };

      template <class F>
      struct create_function_impl<5, F> {
        int operator()(sqlite3* db, void* fh, char const* name) {
          using FT = function_traits<F>;
          using R = typename FT::return_type;
          using P1 = typename FT::template argument<0>::type;
          using P2 = typename FT::template argument<1>::type;
          using P3 = typename FT::template argument<2>::type;
          using P4 = typename FT::template argument<3>::type;
          using P5 = typename FT::template argument<4>::type;

          return sqlite3_create_function(db, name, 5, SQLITE_UTF8, fh,
                                         functionx_impl<R, P1, P2, P3, P4, P5>,
                                         0, 0);
        }
      };

     private:
      sqlite3* db_;

      std::map<std::string, pfunction_base> fh_;
    };

    namespace
    {
      template <class T>
      void step0_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values)
      {
        context c(ctx, nargs, values);
        T* t = static_cast<T*>(c.aggregate_data(sizeof(T)));
        if (c.aggregate_count() == 1) new (t) T;
        t->step();
      }

      template <class T, class P1>
      void step1_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values)
      {
        context c(ctx, nargs, values);
        T* t = static_cast<T*>(c.aggregate_data(sizeof(T)));
        if (c.aggregate_count() == 1) new (t) T;
        t->step(c.context::get<P1>(0));
      }

      template <class T, class P1, class P2>
      void step2_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values)
      {
        context c(ctx, nargs, values);
        T* t = static_cast<T*>(c.aggregate_data(sizeof(T)));
        if (c.aggregate_count() == 1) new (t) T;
        t->step(c.context::get<P1>(0), c.context::get<P2>(1));
      }

      template <class T, class P1, class P2, class P3>
      void step3_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values)
      {
        context c(ctx, nargs, values);
        T* t = static_cast<T*>(c.aggregate_data(sizeof(T)));
        if (c.aggregate_count() == 1) new (t) T;
        t->step(c.context::get<P1>(0), c.context::get<P2>(1), c.context::get<P3>(2));
      }

      template <class T, class P1, class P2, class P3, class P4>
      void step4_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values)
      {
        context c(ctx, nargs, values);
        T* t = static_cast<T*>(c.aggregate_data(sizeof(T)));
        if (c.aggregate_count() == 1) new (t) T;
        t->step(c.context::get<P1>(0), c.context::get<P2>(1), c.context::get<P3>(2), c.context::get<P4>(3));
      }

      template <class T, class P1, class P2, class P3, class P4, class P5>
      void step5_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values)
      {
        context c(ctx, nargs, values);
        T* t = static_cast<T*>(c.aggregate_data(sizeof(T)));
        if (c.aggregate_count() == 1) new (t) T;
        t->step(c.context::get<P1>(0), c.context::get<P2>(1), c.context::get<P3>(2), c.context::get<P4>(3), c.context::get<P5>(4));
      }

      template <class T>
      void finishN_impl(sqlite3_context* ctx)
      {
        context c(ctx);
        T* t = static_cast<T*>(c.aggregate_data(sizeof(T)));
        c.result(t->finish());
        t->~T();
      }

    }

    class aggregate : noncopyable
    {
     public:
      using function_handler = std::function<void (context&)>;
      using pfunction_base = std::shared_ptr<void>;

      explicit aggregate(database& db);

      int create(char const* name, function_handler s, function_handler f, int nargs = 1);

      template <class T>
      int create(char const* name) {
        return sqlite3_create_function(db_, name, 0, SQLITE_UTF8, 0, 0, step0_impl<T>, finishN_impl<T>);
      }

      template <class T, class P1>
      int create(char const* name) {
        return sqlite3_create_function(db_, name, 1, SQLITE_UTF8, 0, 0, step1_impl<T, P1>, finishN_impl<T>);
      }

      template <class T, class P1, class P2>
      int create(char const* name) {
        return sqlite3_create_function(db_, name, 2, SQLITE_UTF8, 0, 0, step2_impl<T, P1, P2>, finishN_impl<T>);
      }

      template <class T, class P1, class P2, class P3>
      int create(char const* name) {
        return sqlite3_create_function(db_, name, 3, SQLITE_UTF8, 0, 0, step3_impl<T, P1, P2, P3>, finishN_impl<T>);
      }

      template <class T, class P1, class P2, class P3, class P4>
      int create(char const* name) {
        return sqlite3_create_function(db_, name, 4, SQLITE_UTF8, 0, 0, step4_impl<T, P1, P2, P3, P4>, finishN_impl<T>);
      }

      template <class T, class P1, class P2, class P3, class P4, class P5>
      int create(char const* name) {
        return sqlite3_create_function(db_, name, 5, SQLITE_UTF8, 0, 0, step5_impl<T, P1, P2, P3, P4, P5>, finishN_impl<T>);
      }

     private:
      sqlite3* db_;

      std::map<std::string, std::pair<pfunction_base, pfunction_base> > ah_;
    };

  } // namespace ext

} // namespace sqlite3pp

#endif
