////////////////////////////////////////////////////////////////////////
// This file is part of Grappa, a system for scaling irregular
// applications on commodity clusters. 

// Copyright (C) 2010-2014 University of Washington and Battelle
// Memorial Institute. University of Washington authorizes use of this
// Grappa software.

// Grappa is free software: you can redistribute it and/or modify it
// under the terms of the Affero General Public License as published
// by Affero, Inc., either version 1 of the License, or (at your
// option) any later version.

// Grappa is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// Affero General Public License for more details.

// You should have received a copy of the Affero General Public
// License along with this program. If not, you may obtain one from
// http://www.affero.org/oagpl.html.
////////////////////////////////////////////////////////////////////////
#pragma once

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <vector>
#include "Semaphore.hpp"


namespace Grappa {
namespace impl {
    

template< typename T,
          typename Semaphore = CountingSemaphore, 
          int max_count = CountingSemaphore::max_value >
class ReusePool {
private:
  CountingSemaphore s_;
  T * ptrs_[ max_count ];

public:
  ReusePool() 
    : ptrs_()
    , s_(0)
  { }

  bool available() {
    return s_.get_value() > 0;
  }

  int64_t count() {
    return s_.get_value();
  }

  T * block_until_pop() {
    DVLOG(5) << __PRETTY_FUNCTION__ << "/" << this << ": blocking until pop with " << s_.get_value() << " now";
    s_.decrement();
    T * result = ptrs_[s_.get_value()];
    DVLOG(5) << __PRETTY_FUNCTION__ << "/" << this << ": finished blocking until pop with " << s_.get_value() << "/" << result;
    return result;
  }

  T * try_pop() {
    DVLOG(5) << __PRETTY_FUNCTION__ << "/" << this << ": trying to pop with " << s_.get_value() << " now";
    if( s_.try_decrement() ) {
      T * t = ptrs_[ s_.get_value() ]; 
      DVLOG(5) << __PRETTY_FUNCTION__ << "/" << this << ": succeeded; popping " << t << " with " << s_.get_value() << " now";
      return t;
    } else {
      return NULL;
    }
  }

  void push( T * buf ) {
    DVLOG(5) << __PRETTY_FUNCTION__ << "/" << this << ": pushing " << buf << " with " << s_.get_value() << " already";
    CHECK_LT( s_.get_value(), max_count ) << "Can't check in buffer; maximum is " << max_count;
    ptrs_[ s_.get_value() ] = buf;
    s_.increment();
  }

  bool try_push( T * buf ) {
    DVLOG(5) << __PRETTY_FUNCTION__ << "/" << this << ": trying to push " << buf << " with " << s_.get_value() << " already";
    if( s_.get_value() < max_count ) {
      push(buf);
      DVLOG(5) << __PRETTY_FUNCTION__ << "/" << this << ": succeeded; pushed " << buf << " with " << s_.get_value() << " now";
      return true;
    } else {
      return false;
    }
  }
};


}
}
