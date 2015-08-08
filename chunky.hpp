#ifndef CHUNKY_HPP
#define CHUNKY_HPP
/*
Copyright 2015 Shoestring Research, LLC.  All rights reserved.

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

#include <algorithm>
#include <deque>
#include <memory>
#include <regex>
#include <string>
#include <utility>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/asio.hpp>
#include <boost/format.hpp>
#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/utility.hpp>

namespace chunky {
   namespace detail {
      struct CaselessCompare {
         bool operator()(const std::string& a,const std::string& b) const {
            return boost::ilexicographical_compare(a,b);
         }
      };
   }
   
   enum errors {
      invalid_request_line = 1,
      invalid_request_header,
      unsupported_http_version,
      invalid_content_length,
      invalid_chunk_length
   };
   
   inline boost::system::error_code make_error_code(errors e) {
      class error_category : public boost::system::error_category
      {
      public:
         const char* name() const noexcept {
            return "chunky";
         }

         std::string message(int value) const {
            switch (value) {
            case invalid_request_line:
               return "Invalid request line";
            case invalid_request_header:
               return "Invalid request header";
            case unsupported_http_version:
               return "Unsupported HTTP version";
            case invalid_content_length:
               return "Invalid Content-Length";
            case invalid_chunk_length:
               return "Invalid chunk length";
            default:
               return "chunky error";
            }
         }
      };
      static error_category category;

      return boost::system::error_code(
         static_cast<int>(e), category);
   }

   // This is a wrapper for a boost::asio stream class (e.g.
   // boost::asio::ip::tcp::socket). It provides three features:
   //
   // 1. Asynchronous operations are thread-safe via a strand.
   // 2. A put back buffer is available for overread data.
   // 3. Stream lifetime is ensured (via shared_ptr) for asynchronous
   //    operations.
   template<typename T>
   class Stream : public std::enable_shared_from_this<Stream<T> >
                , boost::noncopyable {
   public:
      typedef T stream_t;
      
      virtual ~Stream() {}

      stream_t& stream() {
         return stream_;
      }
      
      boost::asio::io_service& get_io_service() {
         return stream_.get_io_service();
      }

      template<typename MutableBufferSequence, typename ReadHandler>
      void async_read_some(
         const MutableBufferSequence& buffers,
         ReadHandler&& handler) {
         if (!readBuffer_.empty()) {
            // Call the synchronous function. It won't block because
            // the data are buffered.
            boost::system::error_code error;
            const auto nBytes = read_some(buffers, error);
            strand_.post([=]() mutable {
                  handler(error, nBytes);
               });
         }
         else {
            auto this_ = this->shared_from_this();
            strand_.dispatch([=]() mutable {
                  // Wrapping the handler is unnecessary because the
                  // call is not a composed operation.
                  this_->stream_.async_read_some(buffers, handler);
               });
         }
      }

      template<typename ConstBufferSequence, typename WriteHandler>
      void async_write_some(
         const ConstBufferSequence& buffers,
         WriteHandler&& handler) {
         auto this_ = this->shared_from_this();
         strand_.dispatch([=]() mutable {
               // Wrapping the handler is unnecessary because the call
               // is not a composed operation.
               this_->stream_.async_write_some(buffers, handler);
            });
      }

      template<typename MutableBufferSequence>
      size_t read_some(
         const MutableBufferSequence& buffers,
         boost::system::error_code& error) {
         if (!readBuffer_.empty()) {
            const auto nBytes = std::min(readBuffer_.size(), boost::asio::buffer_size(buffers));
            const auto iBegin = readBuffer_.begin();
            const auto iEnd = iBegin + nBytes;
            std::copy(iBegin, iEnd, boost::asio::buffers_begin(buffers));
            readBuffer_.erase(iBegin, iEnd);
            return nBytes;
         }
         else
            return stream_.read_some(buffers, error);
      }
      
      template<typename MutableBufferSequence>
      size_t read_some(
         const MutableBufferSequence& buffers) {
         boost::system::error_code error;
         const auto nBytes = read_some(buffers, error);
         if (error)
            throw boost::system::system_error(error);
         return nBytes;
      }

      template<typename ConstBufferSequence>
      size_t write_some(
         const ConstBufferSequence& buffers,
         boost::system::error_code& error) {
         return stream_.write_some(buffers, error);
      }

      template<typename ConstBufferSequence>
      size_t write_some(
         const ConstBufferSequence& buffers) {
         boost::system::error_code error;
         const auto nBytes = write_some(buffers, error);
         if (error)
            throw boost::system::system_error(error);
         return nBytes;
      }
      
      template<typename ConstBufferSequence>
      void put_back(const ConstBufferSequence& buffers) {
         readBuffer_.insert(
            readBuffer_.begin(),
            boost::asio::buffers_begin(buffers), boost::asio::buffers_end(buffers));
      }

   protected:
      template<typename A0>
      Stream(A0&& a0)
         : stream_(std::forward<A0>(a0))
         , strand_(stream_.get_io_service()) {
      }
      
   private:
      T stream_;
      boost::asio::io_service::strand strand_;
      std::deque<char> readBuffer_;
   };

   // This is a wrapped boost::asio TCP stream.
   class TCP : public Stream<boost::asio::ip::tcp::socket> {
   public:
      template<typename CreateHandler>
      static void async_connect(
         boost::asio::ip::tcp::acceptor& acceptor,
         CreateHandler handler) {
         std::shared_ptr<TCP> tcp(new TCP(acceptor.get_io_service()));
         acceptor.async_accept(
            tcp->stream(),
            [=](const boost::system::error_code& error) mutable {
               if (error)
                  tcp.reset();
               handler(error, tcp);
            });
      }

      static std::shared_ptr<TCP> create(boost::asio::ip::tcp::socket&& socket) {
         return std::shared_ptr<TCP>(new TCP(std::move(socket)));
      }

      ~TCP() {
         if (stream().is_open()) {
            boost::system::error_code error;
            stream().shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
            stream().close(error);
         }
      }
   private:
      TCP(boost::asio::io_service& io)
         : Stream<boost::asio::ip::tcp::socket>(io) {
      }

      TCP(boost::asio::ip::tcp::socket&& socket)
         : Stream<boost::asio::ip::tcp::socket>(std::move(socket)) {
      }
   };

   template<typename T>
   class HTTPTemplate : public std::enable_shared_from_this<HTTPTemplate<T> >
                      , boost::noncopyable {
   public:
      typedef std::map<std::string, std::string, detail::CaselessCompare> Headers;

      template<typename CreateHandler>
      static void async_create(
         const std::shared_ptr<T>& stream,
         CreateHandler&& handler) {
         std::shared_ptr<HTTPTemplate> http(new HTTPTemplate(stream));
         http->read_request_line([=](const boost::system::error_code& error) {
               if (error) {
                  handler(error, http);
                  return;
               }

               http->read_request_headers([=](const boost::system::error_code& error) {
                     if (error) {
                        handler(error, http);
                        return;
                     }

                     http->read_length([=](const boost::system::error_code& error) {
                           handler(error, http);
                        });
                  });
            });
      }

      boost::asio::io_service& get_io_service() {
         return stream()->get_io_service();
      }
      
      template<typename MutableBufferSequence, typename ReadHandler>
      void async_read_some(MutableBufferSequence&& buffers, ReadHandler&& handler) {
         // Take data from the streambuf first.
         size_t nBytesRead = 0;
         const auto bufferSize = boost::asio::buffer_size(buffers);
         if (streambuf_.size()) {
            auto nBytes = boost::asio::buffer_copy(buffers, streambuf_.data(), requestBytes_);
            streambuf_.consume(nBytes);
            requestBytes_ -= nBytes;
            nBytesRead += nBytes;
         }

         boost::asio::async_read(
            *stream(), buffers, boost::asio::transfer_exactly(nBytesRead ? 0 : requestBytes_),
            [=](const boost::system::error_code& error, size_t nBytes) mutable {
               requestBytes_ -= nBytes;
               nBytesRead += nBytes;
               if (error) {
                  handler(error, nBytesRead);
                  return;
               }

               // Read the chunk delimiter and next chunk header if chunked.
               if (bufferSize && requestChunksPending_ && !requestBytes_) {
                  async_get_line([=](const boost::system::error_code& error, const std::string& s) mutable {
                        assert(s.empty());
                        if (error) {
                           handler(error, nBytesRead);
                           return;
                        }
                        
                        read_chunk_header([=](const boost::system::error_code& error) mutable {
                              handler(error, nBytesRead);
                           });
                     });
               }
               else {
                  boost::system::error_code error;
                  if (nBytesRead == 0 && bufferSize > 0)
                     error = boost::asio::error::make_error_code(boost::asio::error::eof);
                  handler(error, nBytesRead);
               }
            });
      }

      template<typename MutableBufferSequence>
      size_t read_some(MutableBufferSequence&& buffers) {
         const auto bufferSize = boost::asio::buffer_size(buffers);
         auto nBytes = std::min(requestBytes_, bufferSize);
         if (nBytes == 0 && bufferSize > 0)
            throw boost::system::system_error(boost::asio::error::make_error_code(boost::asio::error::eof));
         
         // Take data from the streambuf first.
         if (streambuf_.size()) {
            nBytes = boost::asio::buffer_copy(buffers, streambuf_.data(), nBytes);
            streambuf_.consume(nBytes);
         }
         else
            nBytes = boost::asio::read(*stream(), buffers, boost::asio::transfer_exactly(nBytes));
         
         // Read the chunk delimiter and next chunk header if chunked.
         requestBytes_ -= nBytes;
         if (bufferSize && requestChunksPending_ && !requestBytes_) {
            get_line();
            read_chunk_header();
         }

         return nBytes;
      }
      
      template<typename MutableBufferSequence>
      size_t read_some(MutableBufferSequence&& buffers, boost::system::error_code& error) {
         try {
            return read_some(buffers);
         }
         catch (const boost::system::system_error& e) {
            error = e.code();
            return 0;
         }
      }

      template<typename ConstBufferSequence, typename WriteHandler>
      void async_write_some(ConstBufferSequence&& buffers, WriteHandler&& handler) {
         // Write status, headers, and chunk prefix if necessary.
         auto nBytes = boost::asio::buffer_size(buffers);
         auto prefix = std::make_shared<std::string>(prepare_write_prefix(nBytes));
         if (!prefix->empty()) {
            boost::asio::async_write(
               *stream(), boost::asio::buffer(*prefix),
               [=](const boost::system::error_code& error, size_t) mutable {
                  prefix.get();
                  if (error) {
                     handler(error, 0);
                     return;
                  }
                  
                  async_write_some_1(buffers, handler);
               });
         }
         else
            async_write_some_1(buffers, handler);
      }
      
      template<typename ConstBufferSequence>
      size_t write_some(ConstBufferSequence&& buffers, boost::system::error_code& error) {
         auto nBytes = boost::asio::buffer_size(buffers);

         // Write status, headers, and chunk prefix if necessary.
         const auto prefix = prepare_write_prefix(nBytes);
         if (!prefix.empty()) {
            boost::asio::write(*stream(), boost::asio::buffer(prefix), error);
            if (error)
               return 0;
         }

         // Write user data.
         if (nBytes) {
            nBytes = boost::asio::write(*stream(), buffers, error);
            if (error)
               return nBytes;
         }

         // Write chunk suffix if necessary.
         const auto suffix = prepare_write_suffix(nBytes);
         if (!suffix.empty()) {
            boost::asio::write(*stream(), boost::asio::buffer(suffix), error);
            if (error)
               return 0;
         }

         responseBytes_ += nBytes;
         return nBytes;
      }
      
      template<typename ConstBufferSequence>
      size_t write_some(ConstBufferSequence&& buffers) {
         boost::system::error_code error;
         size_t nBytes = write_some(buffers, error);
         if (error)
            throw boost::system::system_error(error);
         return nBytes;
      }

      // The handler will be called as:
      //   handler(const boost::system::error_code&);
      template<typename FinishHandler>
      void async_finish(FinishHandler&& handler) {
         flush_input([=](const boost::system::error_code& error) mutable {
               if (error) {
                  handler(error);
                  return;
               }
               
               // Replace any unused bytes read by get_line().
               if (auto unused = streambuf_.size()) {
                  stream()->put_back(streambuf_.data());
                  streambuf_.consume(unused);
               }

               // Use Content-Length for empty body.
               if (!responseBytes_)
                  response_headers()["Content-Length"] = "0";

               // Output final empty chunk.
               async_write_some(
                  boost::asio::null_buffers(),
                  [=](const boost::system::error_code& error, size_t) mutable {
                     if (error) {
                        handler(error);
                        return;
                     }

                     if (responseChunked_) {
                        // Output trailers.
                        std::string s;
                        boost::iostreams::filtering_ostream os(boost::iostreams::back_inserter(s));
                        write_headers(os, [](const std::string& s) {
                              if (!s.empty() && s[0] == '/')
                                 return s.substr(1);
                              return std::string();
                           });
                        os.reset();

                        boost::asio::async_write(
                           *stream(), boost::asio::buffer(s),
                           [=](const boost::system::error_code& error, size_t) mutable {
                              handler(error);
                           });
                     }
                     else
                        handler(error);
                  });
            });
      }
      
      void finish() {
         flush_input();

         // Replace any unused bytes read by get_line().
         if (auto unused = streambuf_.size()) {
            stream()->put_back(streambuf_.data());
            streambuf_.consume(unused);
         }

         // Use Content-Length for empty body.
         if (!responseBytes_)
            response_headers()["Content-Length"] = "0";
         
         // Output final empty chunk.
         write_some(boost::asio::null_buffers());
         
         if (responseChunked_) {
            // Output trailers.
            std::string s;
            boost::iostreams::filtering_ostream os(boost::iostreams::back_inserter(s));
            write_headers(os, [](const std::string& s) {
                  if (!s.empty() && s[0] == '/')
                     return s.substr(1);
                  return std::string();
               });
            os.reset();

            boost::asio::write(*stream(), boost::asio::buffer(s));
         }
      }
      
      void finish(boost::system::error_code& error) {
         try {
            finish();
         }
         catch (const boost::system::system_error& e) {
            error = e.code();
         }
      }

      std::shared_ptr<T>& stream() {
         return stream_;
      }

      const std::string& request_method() const { return requestMethod_; }
      const std::string& request_version() const { return requestVersion_; }
      const std::string& request_resource() const { return requestResource_; }
      const Headers& request_headers() const { return requestHeaders_; }

      unsigned int& response_status() { return responseStatus_; }
      Headers& response_headers() { return responseHeaders_; }
      
   private:
      std::shared_ptr<T> stream_;
      boost::asio::streambuf streambuf_;
      
      std::string requestMethod_;
      std::string requestVersion_;
      std::string requestResource_;
      Headers requestHeaders_;

      size_t requestBytes_;
      bool requestChunksPending_;
      
      unsigned int responseStatus_;
      Headers responseHeaders_;

      size_t responseBytes_;
      bool responseChunked_;

      HTTPTemplate(const std::shared_ptr<T>& stream)
         : stream_(stream)
         , requestBytes_(0)
         , requestChunksPending_(false)
         , responseBytes_(0)
         , responseChunked_(false) {
      }

      static const std::string& crlf() {
         static const std::string s("\r\n");
         return s;
      }
      
      template<typename Handler>
      void async_get_line(Handler&& handler) {
         boost::asio::async_read_until(
            *stream(), streambuf_, crlf(),
            [this, handler](const boost::system::error_code& error, size_t nBytes) mutable {
               if (!error) {
                  auto i = boost::asio::buffers_begin(streambuf_.data());
                  std::string s(i, i + (nBytes - crlf().size()));
                  streambuf_.consume(nBytes);
                  handler(error, std::move(s));
               }
               else
                  handler(error, std::string());
            });
      }

      std::string get_line() {
         auto nBytes = boost::asio::read_until(
            *stream(), streambuf_, crlf());

         auto i = boost::asio::buffers_begin(streambuf_.data());
         std::string s(i, i + nBytes - crlf().size());
         streambuf_.consume(nBytes);
         return s;
      }

      // Several helper functions below take a std::function callback.
      // The callback may be empty for synchronous use (throwing an
      // exception on an error); otherwise the callback is invoked
      // asynchronously with the status.
      typedef std::function<void(const boost::system::error_code&)> Handler;
      
      void read_request_line(const Handler& handler = Handler()) {
         if (handler) {
            async_get_line([this, handler](boost::system::error_code error, std::string&& s) {
                  if (!error)
                     error = process_request_line(s);
                  handler(error);
               });
         }
         else {
            const std::string s = get_line();
            if (boost::system::error_code error = process_request_line(s))
               throw boost::system::system_error(error);
         }
      }

      // This helper parses an HTTP 1.1 request line.
      boost::system::error_code process_request_line(const std::string& s) {
         std::smatch requestMatch;
         static const std::regex requestRegex("([-!#$%^&*+._'`|~0-9A-Za-z]+) (\\S+) (HTTP/\\d\\.\\d)");
         if (std::regex_match(s, requestMatch, requestRegex)) {
            requestMethod_   = requestMatch[1];
            requestResource_ = requestMatch[2];
            requestVersion_  = requestMatch[3];

            if (requestVersion_ != "HTTP/1.1")
               return make_error_code(unsupported_http_version);
         }
         else
            return make_error_code(invalid_request_line);

         return boost::system::error_code();
      }

      // This helper reads headers from the stream.
      void read_request_headers(const Handler& handler = Handler()) {
         if (handler) {
            async_get_line([this, handler](boost::system::error_code error, const std::string& s) {
                  if (!s.empty() && !(error = process_request_header(s)))
                     read_request_headers(handler);
                  else
                     handler(error);
               });
         }
         else {
            for (auto s = get_line(); !s.empty(); s = get_line()) {
               if (auto error = process_request_header(s))
                  throw boost::system::system_error(error);
            }
         }
      }

      // This helper parses a header.
      boost::system::error_code process_request_header(const std::string& s) {
         const auto colon = s.find_first_of(':');
         if (colon == std::string::npos)
            return make_error_code(invalid_request_header);

         std::string key = s.substr(0, colon);
         std::string value = s.substr(colon + 1);
         boost::algorithm::trim_left(value);

         // Coalesce values with the same key.
         const auto i = requestHeaders_.find(key);
         if (i != requestHeaders_.end()) {
            i->second += ", ";
            i->second += value;
         }
         else
            requestHeaders_.insert({{std::move(key), std::move(value)}});

         return boost::system::error_code();
      }

      // This helper sets requestBytes_, which is the number of request
      // body bytes available to be read. It handles both Content-Length
      // and Transfer-Encoding (chunked).
      void read_length(const Handler& handler = Handler()) {
         try {
            auto contentLength = requestHeaders_.find("content-length");
            if (contentLength != requestHeaders_.end())
               requestBytes_ = static_cast<size_t>(std::stoul(contentLength->second));
         }
         catch (...) {
            // std::stoul() threw on the Content-Length header.
            auto error = make_error_code(invalid_content_length);
            if (handler) {
               handler(error);
               return;
            }
            throw boost::system::system_error(error);
         }

         auto transferEncoding = requestHeaders_.find("transfer-encoding");
         if (transferEncoding != requestHeaders_.end() &&
             transferEncoding->second != "identity") {
            requestBytes_ = 0U;
            requestChunksPending_ = true;
            read_chunk_header(handler);
         }
         else if (handler)
            handler(boost::system::error_code());
      }

      // This helper reads a chunk length header.
      void read_chunk_header(const Handler& handler = Handler()) {
         assert(requestBytes_ == 0);
         assert(requestChunksPending_);
         if (handler) {
            async_get_line([this, handler](boost::system::error_code error, const std::string& s) {
                  if (error) {
                     handler(error);
                     return;
                  }

                  try {
                     process_chunk_header(s);
                  }
                  catch (...) {
                     // std::stoul() threw on the chunk header.
                     handler(make_error_code(invalid_chunk_length));
                     return;
                  }

                  if (!requestChunksPending_)
                     read_request_headers(handler);
                  else
                     handler(error);
               });
         }
         else {
            const std::string s = get_line();
            process_chunk_header(s);
            if (!requestChunksPending_)
               read_request_headers();
         }
      }

      // This helper parses a chunk length header.
      void process_chunk_header(const std::string& s) {
         // Chunk header begins with a hexadecimal chunk length.
         requestBytes_ = static_cast<size_t>(std::stoul(s, nullptr, 16));
         if (!requestBytes_)
            requestChunksPending_ = false;
      }

      // This helper ensures that the entire request has been consumed
      // from the stream.
      void flush_input(const Handler& handler = Handler()) {
         if (handler) {
            if (requestBytes_) {
               const auto n = std::min(requestBytes_, decltype(requestBytes_)(65536));
               auto b = std::make_shared<std::vector<char> >(n);
               boost::asio::async_read(
                  *this, boost::asio::buffer(*b),
                  [=](const boost::system::error_code& error, size_t nBytes) {
                     b.get();
                     if (error) {
                        handler(error);
                        return;
                     }

                     flush_input(handler);
                  });
            }
            else
               handler(boost::system::error_code());
         }
         else {
            while (requestBytes_) {
               const auto n = std::min(requestBytes_, decltype(requestBytes_)(65536));
               std::vector<char> b(n);
               boost::asio::read(*this, boost::asio::buffer(b));
            }
         }
      }

      template<typename ConstBufferSequence, typename WriteHandler>
      void async_write_some_1(ConstBufferSequence&& buffers, WriteHandler&& handler) {
         // Write user data.
         if (boost::asio::buffer_size(buffers)) {
            boost::asio::async_write(
               *stream(), buffers,
               [=](const boost::system::error_code& error, size_t nBytes) mutable {
                  if (error) {
                     handler(error, nBytes);
                     return;
                  }

                  async_write_some_2(buffers, handler);
               });
         }
         else
            async_write_some_2(buffers, handler);
      }
         
      template<typename ConstBufferSequence, typename WriteHandler>
      void async_write_some_2(ConstBufferSequence&& buffers, WriteHandler&& handler) {
         // Write chunk suffix if necessary.
         const auto nBytes = boost::asio::buffer_size(buffers);
         auto suffix = std::make_shared<std::string>(prepare_write_suffix(nBytes));
         if (!suffix->empty()) {
            boost::asio::async_write(
               *stream(), boost::asio::buffer(*suffix),
               [=](const boost::system::error_code& error, size_t) mutable {
                  suffix.get();
                  responseBytes_ += nBytes;
                  handler(error, nBytes);
               });
         }
         else
            handler(boost::system::error_code(), nBytes);
      }
      
      std::string prepare_write_prefix(size_t nBytes) {
         std::string s;
         boost::iostreams::filtering_ostream os(boost::iostreams::back_inserter(s));
         if (responseBytes_ == 0) {
            // Set Date header if not already present.
            if (response_headers().find("date") == response_headers().end()) {
               std::time_t t;
               std::time(&t);
               std::tm tm = *std::gmtime(&t);
               char s[30];
               auto n = strftime(s, sizeof(s), "%a, %d %b %Y %T GMT", &tm);
               response_headers()["Date"] = std::string(s, n);
            }
            
            // Determine whether to use chunked transfer.
            auto transferEncoding = response_headers().find("transfer-encoding");
            if (transferEncoding != response_headers().end() &&
                transferEncoding->second != "identity") {
               responseChunked_ = true;
               response_headers().erase("content-length");
            }
            else if (response_headers().count("content-length") == 0 && nBytes) {
               responseChunked_ = true;
               response_headers()["Transfer-Encoding"] = "chunked";
            }

            write_status(os);
            write_headers(os, [](const std::string& s) -> std::string {
                  if (!s.empty() && s[0] != '/')
                     return s;
                  return std::string();
               });
         }

         if (responseChunked_)
            os << boost::format("%x\r\n") % nBytes;
         
         os.reset();
         return s;
      }

      std::string prepare_write_suffix(size_t nBytes) {
         // Add crlf to all chunks except the final one.
         return (responseChunked_ && nBytes) ? crlf() : std::string();
      }

      void write_status(std::ostream& os) {
         static std::map<unsigned int, std::string> reasons = {
            { 100, "Continue" },
            { 101, "Switching Protocols" },
            { 200, "OK" },
            { 201, "Created" },
            { 202, "Accepted" },
            { 203, "Non-Authoritative Information" },
            { 204, "No Content" },
            { 205, "Reset Content" },
            { 206, "Partial Content" },
            { 300, "Multiple Choices" },
            { 301, "Moved Permanently" },
            { 302, "Found" },
            { 303, "See Other" },
            { 304, "Not Modified" },
            { 305, "Use Proxy" },
            { 307, "Temporary Redirect" },
            { 400, "Bad Request" },
            { 401, "Unauthorized" },
            { 402, "Payment Required" },
            { 403, "Forbidden" },
            { 404, "Not Found" },
            { 405, "Method Not Allowed" },
            { 406, "Not Acceptable" },
            { 407, "Proxy Authentication Required" },
            { 408, "Request Timeout" },
            { 409, "Conflict" },
            { 410, "Gone" },
            { 411, "Length Required" },
            { 412, "Precondition Failed" },
            { 413, "Payload Too Large" },
            { 414, "URI Too Long" },
            { 415, "Unsupported Media Type" },
            { 416, "Range Not Satisfiable" },
            { 417, "Expectation Failed" },
            { 426, "Upgrade Required" },
            { 500, "Internal Server Error" },
            { 501, "Not Implemented" },
            { 502, "Bad Gateway" },
            { 503, "Service Unavailable" },
            { 504, "Gateway Timeout" },
            { 505, "HTTP Version Not Supported" }
         };

         auto reason = reasons.find(responseStatus_);
         os << boost::format("HTTP/1.1 %d %s\r\n")
            % responseStatus_
            % (reason != reasons.end() ? reason->second : std::string());
      }

      template<typename Filter>
      void write_headers(std::ostream& os, Filter filter) {
         for (const auto& value : response_headers()) {
            const auto key = filter(value.first);
            if (!key.empty()) {
               os << boost::format("%s: %s\r\n")
                  % key
                  % value.second;
            }
         }
         os << crlf();
      }
   };
   
   typedef HTTPTemplate<TCP> HTTP;
}

#endif // CHUNKY_HPP
