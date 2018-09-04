// Copyright (C) 2018 Alain Miniussi <alain.miniussi@oca.eu>.

// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

// Request implementation dtails

// This header should be included only after the communicator and request 
// classes has been defined.
#ifndef BOOST_MPI_REQUEST_HANDLERS_HPP
#define BOOST_MPI_REQUEST_HANDLERS_HPP

#include <boost/mpi/skeleton_and_content_types.hpp>

namespace boost { namespace mpi {


namespace detail {
  /**
   * Internal data structure that stores everything required to manage
   * the receipt of serialized data via a request object.
   */
  template<typename T>
  struct serialized_irecv_data {
    serialized_irecv_data(const communicator& comm, T& value)
      : m_ia(comm), m_value(value) {}

    void deserialize(status& stat) 
    { 
      m_ia >> m_value; 
      stat.m_count = 1;
    }

    std::size_t     m_count;
    packed_iarchive m_ia;
    T&              m_value;
  };

  template<>
  struct serialized_irecv_data<packed_iarchive>
  {
    serialized_irecv_data(communicator const&, packed_iarchive& ia) : m_ia(ia) { }

    void deserialize(status&) { /* Do nothing. */ }

    std::size_t      m_count;
    packed_iarchive& m_ia;
  };

  /**
   * Internal data structure that stores everything required to manage
   * the receipt of an array of serialized data via a request object.
   */
  template<typename T>
  struct serialized_array_irecv_data
  {
    serialized_array_irecv_data(const communicator& comm, T* values, int n)
      : ia(comm), values(values), n(n) {}

    void deserialize(status& stat);

    std::size_t count;
    packed_iarchive ia;
    T* values;
    int n;
  };

  template<typename T>
  void serialized_array_irecv_data<T>::deserialize(status& stat)
  {
    // Determine how much data we are going to receive
    int count;
    ia >> count;
    
    // Deserialize the data in the message
    boost::serialization::array_wrapper<T> arr(values, count > n? n : count);
    ia >> arr;
    
    if (count > n) {
      boost::throw_exception(
        std::range_error("communicator::recv: message receive overflow"));
    }
    
    stat.m_count = count;
  }

  /**
   * Internal data structure that stores everything required to manage
   * the receipt of an array of primitive data but unknown size.
   * Such an array can have been send with blocking operation and so must
   * be compatible with the (size_t,raw_data[]) format.
   */
  template<typename T, class A>
  struct dynamic_array_irecv_data
  {
    BOOST_STATIC_ASSERT_MSG(is_mpi_datatype<T>::value, "Can only be specialized for MPI datatypes.");

    dynamic_array_irecv_data(std::vector<T,A>& values)
      :  count(-1), values(values) {}

    std::size_t count;
    std::vector<T,A>& values;
  };

  template<typename T>
  struct serialized_irecv_data<const skeleton_proxy<T> >
  {
    serialized_irecv_data(const communicator& comm, skeleton_proxy<T> proxy)
      : m_isa(comm), m_ia(m_isa.get_skeleton()), m_proxy(proxy) { }

    void deserialize(status& stat) 
    { 
      m_isa >> m_proxy.object;
      stat.m_count = 1;
    }

    std::size_t              m_count;
    packed_skeleton_iarchive m_isa;
    packed_iarchive&         m_ia;
    skeleton_proxy<T>        m_proxy;
  };

  template<typename T>
  struct serialized_irecv_data<skeleton_proxy<T> >
    : public serialized_irecv_data<const skeleton_proxy<T> >
  {
    typedef serialized_irecv_data<const skeleton_proxy<T> > inherited;

    serialized_irecv_data(const communicator& comm, const skeleton_proxy<T>& proxy)
      : inherited(comm, proxy) { }
  };
}

struct request::probe_handler : public request::handler {
  probe_handler(communicator const& comm, int source, int tag)
    : m_comm(comm), m_source(source), m_tag(tag) {}
  
  bool active() const { return m_source == MPI_PROC_NULL; }
  optional<MPI_Request&> trivial() { return boost::none; }
  MPI_Request& size_request()    { std::abort(); return *(MPI_Request*)0;}
  MPI_Request& payload_request() { std::abort(); return *(MPI_Request*)0;}

  boost::shared_ptr<void> data() { std::abort(); return boost::shared_ptr<void>(); };
  void     set_data(boost::shared_ptr<void> d) { std::abort(); }
  
  communicator const& m_comm;
  int m_source;
  int m_tag;
};

template<class A>
struct request::dynamic_primitive_array_handler : public request::probe_handler {
  dynamic_primitive_array_handler(communicator const& comm, int source, int tag,
                                A& buffer)
    : probe_handler(comm,source,tag), m_buffer(buffer) {}
  
  typedef typename A::value_type value_type;

  status wait() {
    MPI_Message msg;
    status stat;
    BOOST_MPI_CHECK_RESULT(MPI_Mprobe, (m_source,m_tag,m_comm,&msg,&stat.m_status));
    int count;
    MPI_Datatype datatype = get_mpi_datatype<value_type>();
    BOOST_MPI_CHECK_RESULT(MPI_Get_count, (&stat.m_status, datatype, &count));
    m_buffer.resize(count);
    BOOST_MPI_CHECK_RESULT(MPI_Mrecv, (m_buffer.data(), count, datatype, &msg, &stat.m_status));
    m_source = MPI_PROC_NULL;
    return stat;
  }
  
  optional<status> test() {
    status stat;
    int flag = 0;
    MPI_Message msg;
    BOOST_MPI_CHECK_RESULT(MPI_Improbe, (m_source,m_tag,m_comm,&flag,&msg,&stat.m_status));
    if (flag) {
      int count;
      MPI_Datatype datatype = get_mpi_datatype<value_type>();
      BOOST_MPI_CHECK_RESULT(MPI_Get_count, (&stat.m_status, datatype, &count));
      m_buffer.resize(count);
      BOOST_MPI_CHECK_RESULT(MPI_Mrecv, (m_buffer.data(), count, datatype, &msg, &stat.m_status));
      m_source = MPI_PROC_NULL;
      return stat;
    } else {
      return optional<status>();
    } 
  }

  void cancel() {
    m_source = MPI_PROC_NULL;
  }
  
  A& m_buffer;
};

struct request::legacy_handler : public request::handler {
  template<typename T> legacy_handler(communicator const& comm, int source, int tag, T& value);
  template<typename T> legacy_handler(communicator const& comm, int source, int tag, T* value, int n);
  template<typename T, class A> legacy_handler(communicator const& comm, int source, int tag, std::vector<T,A>& values, mpl::true_ primitive);
  
  void cancel() {
    for (int i = 0; i < 2; ++i) {
      if (m_requests[i] != MPI_REQUEST_NULL) {
        BOOST_MPI_CHECK_RESULT(MPI_Cancel, (m_requests+i));
      }
    }
    m_data.reset();
  }
  
  bool active() const;
  optional<MPI_Request&> trivial();
  
  MPI_Request& size_request() { return m_requests[0]; }
  MPI_Request& payload_request() { return m_requests[1]; }
  
  boost::shared_ptr<void> data() { return m_data; }
  void                    set_data(boost::shared_ptr<void> d) { m_data = d; }
  
  template<class T>  boost::shared_ptr<T>    data() { return boost::static_pointer_cast<T>(m_data); }
  template<class T>  void                    set_data(boost::shared_ptr<T> d) { m_data = d; }
  
  MPI_Request      m_requests[2];
  shared_ptr<void> m_data;
  communicator     m_comm;
  int              m_source;
  int              m_tag;
};

template<typename T>
struct request::legacy_serialized_handler 
  : public request::legacy_handler, protected detail::serialized_irecv_data<T> {
  typedef detail::serialized_irecv_data<T> extra;
  legacy_serialized_handler(communicator const& comm, int source, int tag, T& value)
    : legacy_handler(comm, source, tag, value),
      extra(comm, value)  {
    BOOST_MPI_CHECK_RESULT(MPI_Irecv,
			   (&this->extra::m_count, 1, 
			    get_mpi_datatype(this->extra::m_count),
			    source, tag, comm, &size_request()));
    
  }

  status wait() {
    status stat;
    if (m_requests[1] == MPI_REQUEST_NULL) {
      // Wait for the count message to complete
      BOOST_MPI_CHECK_RESULT(MPI_Wait,
                             (m_requests, &stat.m_status));
      // Resize our buffer and get ready to receive its data
      this->extra::m_ia.resize(this->extra::m_count);
      BOOST_MPI_CHECK_RESULT(MPI_Irecv,
                             (this->extra::m_ia.address(), this->extra::m_ia.size(), MPI_PACKED,
                              stat.source(), stat.tag(), 
                              MPI_Comm(m_comm), m_requests + 1));
    }

    // Wait until we have received the entire message
    BOOST_MPI_CHECK_RESULT(MPI_Wait,
                           (m_requests + 1, &stat.m_status));

    this->deserialize(stat);
    return stat;    
  }
  
  optional<status> test() {
    status stat;
    int flag = 0;
    
    if (m_requests[1] == MPI_REQUEST_NULL) {
      // Check if the count message has completed
      BOOST_MPI_CHECK_RESULT(MPI_Test,
                             (m_requests, &flag, &stat.m_status));
      if (flag) {
        // Resize our buffer and get ready to receive its data
        this->extra::m_ia.resize(this->extra::m_count);
        BOOST_MPI_CHECK_RESULT(MPI_Irecv,
                               (this->extra::m_ia.address(), this->extra::m_ia.size(),MPI_PACKED,
                                stat.source(), stat.tag(), 
                                MPI_Comm(m_comm), m_requests + 1));
      } else
        return optional<status>(); // We have not finished yet
    } 

    // Check if we have received the message data
    BOOST_MPI_CHECK_RESULT(MPI_Test,
                           (m_requests + 1, &flag, &stat.m_status));
    if (flag) {
      this->deserialize(stat);
      return stat;
    } else 
      return optional<status>();
  }
};

template<typename T>
struct request::legacy_serialized_array_handler 
  : public request::legacy_handler {
  legacy_serialized_array_handler(communicator const& comm, int source, int tag, T* values, int n)
    : legacy_handler(comm, source, tag, values, n) {}

  status wait() {
    typedef detail::serialized_array_irecv_data<T> data_t;
    shared_ptr<data_t> data = this->data<data_t>();
    status stat;
    if (m_requests[1] == MPI_REQUEST_NULL) {
      // Wait for the count message to complete
      BOOST_MPI_CHECK_RESULT(MPI_Wait,
                             (m_requests, &stat.m_status));
      // Resize our buffer and get ready to receive its data
      data->ia.resize(data->count);
      BOOST_MPI_CHECK_RESULT(MPI_Irecv,
                             (data->ia.address(), data->ia.size(), MPI_PACKED,
                              stat.source(), stat.tag(), 
                              MPI_Comm(m_comm), m_requests + 1));
    }

    // Wait until we have received the entire message
    BOOST_MPI_CHECK_RESULT(MPI_Wait,
                           (m_requests + 1, &stat.m_status));

    data->deserialize(stat);
    return stat;
  }
  
  optional<status> test() {
    typedef detail::serialized_array_irecv_data<T> data_t;
    shared_ptr<data_t> data = this->data<data_t>();
    status stat;
    int flag = 0;
    
    if (m_requests[1] == MPI_REQUEST_NULL) {
      // Check if the count message has completed
      BOOST_MPI_CHECK_RESULT(MPI_Test,
                             (m_requests, &flag, &stat.m_status));
      if (flag) {
        // Resize our buffer and get ready to receive its data
        data->ia.resize(data->count);
        BOOST_MPI_CHECK_RESULT(MPI_Irecv,
                               (data->ia.address(), data->ia.size(),MPI_PACKED,
                                stat.source(), stat.tag(), 
                                MPI_Comm(m_comm), m_requests + 1));
      } else
        return optional<status>(); // We have not finished yet
    } 

    // Check if we have received the message data
    BOOST_MPI_CHECK_RESULT(MPI_Test,
                           (m_requests + 1, &flag, &stat.m_status));
    if (flag) {
      data->deserialize(stat);
      return stat;
    } else 
      return optional<status>();
  }
};

template<typename T, class A>
struct request::legacy_dynamic_primitive_array_handler 
  : public request::legacy_handler {
  legacy_dynamic_primitive_array_handler(communicator const& comm, int source, int tag, std::vector<T,A>& values)
    : legacy_handler(comm, source, tag, values, mpl::true_()) {}

  status wait() {
    typedef detail::dynamic_array_irecv_data<T,A> data_t;
    shared_ptr<data_t> data = this->data<data_t>();
    status stat;
    if (m_requests[1] == MPI_REQUEST_NULL) {
      // Wait for the count message to complete
      BOOST_MPI_CHECK_RESULT(MPI_Wait,
                             (m_requests, &stat.m_status));
      // Resize our buffer and get ready to receive its data
      data->values.resize(data->count);
      BOOST_MPI_CHECK_RESULT(MPI_Irecv,
                             (&(data->values[0]), data->values.size(), get_mpi_datatype<T>(),
                              stat.source(), stat.tag(), 
                              MPI_Comm(m_comm), m_requests + 1));
    }
    // Wait until we have received the entire message
    BOOST_MPI_CHECK_RESULT(MPI_Wait,
                           (m_requests + 1, &stat.m_status));
    return stat;    
  }

  optional<status> test() {
    typedef detail::dynamic_array_irecv_data<T,A> data_t;
    shared_ptr<data_t> data = this->data<data_t>();
    status stat;
    int flag = 0;
    
    if (m_requests[1] == MPI_REQUEST_NULL) {
      // Check if the count message has completed
      BOOST_MPI_CHECK_RESULT(MPI_Test,
                             (m_requests, &flag, &stat.m_status));
      if (flag) {
        // Resize our buffer and get ready to receive its data
        data->values.resize(data->count);
        BOOST_MPI_CHECK_RESULT(MPI_Irecv,
                               (&(data->values[0]), data->values.size(),MPI_PACKED,
                                stat.source(), stat.tag(), 
                                MPI_Comm(m_comm), m_requests + 1));
      } else
        return optional<status>(); // We have not finished yet
    } 

    // Check if we have received the message data
    BOOST_MPI_CHECK_RESULT(MPI_Test,
                           (m_requests + 1, &flag, &stat.m_status));
    if (flag) {
      return stat;
    } else 
      return optional<status>();
  }
};

struct request::trivial_handler : public request::handler {
  trivial_handler();
  
  status wait();
  optional<status> test();
  void cancel();
  
  bool active() const;
  optional<MPI_Request&> trivial();
  
  MPI_Request& size_request();
  MPI_Request& payload_request();
  
  boost::shared_ptr<void> data();
  void                    set_data(boost::shared_ptr<void> d);
  
  MPI_Request      m_request;
  shared_ptr<void> m_data;
};

struct request::dynamic_handler : public request::handler {
  dynamic_handler();
  
  status wait();
  optional<status> test();
  void cancel();
  
  bool active() const;
  optional<MPI_Request&> trivial();
  
  MPI_Request& size_request();
  MPI_Request& payload_request();
  
  boost::shared_ptr<void> data();
  void                    set_data(boost::shared_ptr<void> d);
  
  MPI_Request      m_requests[2];
  shared_ptr<void> m_data;
};

template<typename T> 
request request::make_serialized(communicator const& comm, int source, int tag, T& value) {
  return request(new legacy_serialized_handler<T>(comm, source, tag, value));
}

template<typename T>
request request::make_serialized_array(communicator const& comm, int source, int tag, T* values, int n) {
  return request(new legacy_serialized_array_handler<T>(comm, source, tag, values, n));
}

template<typename T, class A>
request request::make_dynamic_primitive_array(communicator const& comm, int source, int tag, 
                                              std::vector<T,A>& values) {
  if (probe_messages()) {
    return request(new dynamic_primitive_array_handler<std::vector<T,A> >(comm,source,tag,values));
  } else {
    return request(new legacy_dynamic_primitive_array_handler<T,A>(comm, source, tag, values));
  }
}

template<typename T>
request::legacy_handler::legacy_handler(communicator const& comm, int source, int tag, T& value)
  : m_data(),
    m_comm(comm),
    m_source(source),
    m_tag(tag)
{
  m_requests[0] = MPI_REQUEST_NULL;
  m_requests[1] = MPI_REQUEST_NULL;
}

template<typename T>
request::legacy_handler::legacy_handler(communicator const& comm, int source, int tag, T* values, int n)
  : m_data(new detail::serialized_array_irecv_data<T>(comm, values, n)),
    m_comm(comm),
    m_source(source),
    m_tag(tag)
{
  m_requests[0] = MPI_REQUEST_NULL;
  m_requests[1] = MPI_REQUEST_NULL;
  std::size_t& count = data<detail::serialized_array_irecv_data<T> >()->count;
  BOOST_MPI_CHECK_RESULT(MPI_Irecv,
			 (&count, 1, 
                          get_mpi_datatype(count),
                          source, tag, comm, &size_request()));
}

template<typename T, class A>
request::legacy_handler::legacy_handler(communicator const& comm, int source, int tag, std::vector<T,A>& values, mpl::true_ primitive)
  : m_data(new detail::dynamic_array_irecv_data<T,A>(values)),
    m_comm(comm),
    m_source(source),
    m_tag(tag)
{
  m_requests[0] = MPI_REQUEST_NULL;
  m_requests[1] = MPI_REQUEST_NULL;
  std::size_t& count = data<detail::dynamic_array_irecv_data<T,A> >()->count;
  BOOST_MPI_CHECK_RESULT(MPI_Irecv,
                         (&count, 1, 
                          get_mpi_datatype(count),
                          source, tag, comm, &size_request()));
}

}}

#endif // BOOST_MPI_REQUEST_HANDLERS_HPP