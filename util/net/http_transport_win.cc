// Copyright 2015 The Crashpad Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "util/net/http_transport.h"

#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <wchar.h>
#include <winhttp.h>

#include "base/logging.h"
#include "base/numerics/safe_conversions.h"
#include "base/scoped_generic.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "package.h"
#include "util/file/file_io.h"
#include "util/numeric/safe_assignment.h"
#include "util/net/http_body.h"

namespace crashpad {

namespace {

// PLOG doesn't work for messages from WinHTTP, so we need to use
// FORMAT_MESSAGE_FROM_HMODULE + the dll name manually here.
std::string WinHttpMessage(const char* extra) {
  DWORD error_code = GetLastError();
  char msgbuf[256];
  DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
                FORMAT_MESSAGE_MAX_WIDTH_MASK | FORMAT_MESSAGE_FROM_HMODULE;
  DWORD len = FormatMessageA(flags,
                             GetModuleHandle(L"winhttp.dll"),
                             error_code,
                             0,
                             msgbuf,
                             arraysize(msgbuf),
                             NULL);
  if (!len) {
    return base::StringPrintf("%s: error 0x%x while retrieving error 0x%x",
                              extra,
                              GetLastError(),
                              error_code);
  }
  return base::StringPrintf("%s: %s (0x%x)", extra, msgbuf, error_code);
}

struct ScopedHINTERNETTraits {
  static HINTERNET InvalidValue() {
    return nullptr;
  }
  static void Free(HINTERNET handle) {
    if (handle) {
      if (!WinHttpCloseHandle(handle)) {
        LOG(ERROR) << WinHttpMessage("WinHttpCloseHandle");
      }
    }
  }
};

using ScopedHINTERNET = base::ScopedGeneric<HINTERNET, ScopedHINTERNETTraits>;

class HTTPTransportWin final : public HTTPTransport {
 public:
  HTTPTransportWin();
  ~HTTPTransportWin() override;

  bool ExecuteSynchronously(std::string* response_body) override;

 private:
  DISALLOW_COPY_AND_ASSIGN(HTTPTransportWin);
};

HTTPTransportWin::HTTPTransportWin() : HTTPTransport() {
}

HTTPTransportWin::~HTTPTransportWin() {
}

bool HTTPTransportWin::ExecuteSynchronously(std::string* response_body) {
  ScopedHINTERNET session(
      WinHttpOpen(base::UTF8ToUTF16(PACKAGE_NAME "/" PACKAGE_VERSION).c_str(),
                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                  WINHTTP_NO_PROXY_NAME,
                  WINHTTP_NO_PROXY_BYPASS,
                  0));
  if (!session.get()) {
    LOG(ERROR) << WinHttpMessage("WinHttpOpen");
    return false;
  }

  int timeout_in_ms = static_cast<int>(timeout() * 1000);
  if (!WinHttpSetTimeouts(session.get(),
                          timeout_in_ms,
                          timeout_in_ms,
                          timeout_in_ms,
                          timeout_in_ms)) {
    LOG(ERROR) << WinHttpMessage("WinHttpSetTimeouts");
    return false;
  }

  URL_COMPONENTS url_components = {0};
  url_components.dwStructSize = sizeof(URL_COMPONENTS);
  url_components.dwHostNameLength = 1;
  url_components.dwUrlPathLength = 1;
  url_components.dwExtraInfoLength = 1;
  std::wstring url_wide(base::UTF8ToUTF16(url()));
  // dwFlags = ICU_REJECT_USERPWD fails on XP. See "Community Additions" at:
  // https://msdn.microsoft.com/en-us/library/aa384092.aspx
  if (!WinHttpCrackUrl(
          url_wide.c_str(), 0, 0, &url_components)) {
    LOG(ERROR) << WinHttpMessage("WinHttpCrackUrl");
    return false;
  }
  DCHECK(url_components.nScheme == INTERNET_SCHEME_HTTP ||
         url_components.nScheme == INTERNET_SCHEME_HTTPS);
  std::wstring host_name(url_components.lpszHostName,
                         url_components.dwHostNameLength);
  std::wstring url_path(url_components.lpszUrlPath,
                        url_components.dwUrlPathLength);
  std::wstring extra_info(url_components.lpszExtraInfo,
                          url_components.dwExtraInfoLength);

  ScopedHINTERNET connect(WinHttpConnect(
      session.get(), host_name.c_str(), url_components.nPort, 0));
  if (!connect.get()) {
    LOG(ERROR) << WinHttpMessage("WinHttpConnect");
    return false;
  }

  ScopedHINTERNET request(WinHttpOpenRequest(
      connect.get(),
      base::UTF8ToUTF16(method()).c_str(),
      url_path.c_str(),
      nullptr,
      WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES,
      url_components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE
                                                      : 0));
  if (!request.get()) {
    LOG(ERROR) << WinHttpMessage("WinHttpOpenRequest");
    return false;
  }

  // Add headers to the request.
  //
  // If Content-Length is not provided, implement chunked mode per RFC 7230
  // §4.1.
  //
  // Note that chunked mode can only be used on Vista and later. Otherwise,
  // WinHttpSendRequest() requires a real value for dwTotalLength, used for the
  // Content-Length header. Determining that in the absence of a provided
  // Content-Length would require reading the entire request body before calling
  // WinHttpSendRequest().
  bool chunked = true;
  size_t content_length = 0;
  for (const auto& pair : headers()) {
    if (pair.first == kContentLength) {
      chunked = !base::StringToSizeT(pair.second, &content_length);
      DCHECK(!chunked);
    } else {
      std::wstring header_string = base::UTF8ToUTF16(pair.first) + L": " +
                                   base::UTF8ToUTF16(pair.second) + L"\r\n";
      if (!WinHttpAddRequestHeaders(
              request.get(),
              header_string.c_str(),
              base::checked_cast<DWORD>(header_string.size()),
              WINHTTP_ADDREQ_FLAG_ADD)) {
        LOG(ERROR) << WinHttpMessage("WinHttpAddRequestHeaders");
        return false;
      }
    }
  }

  DWORD content_length_dword;
  if (chunked) {
    const wchar_t kTransferEncodingHeader[] = L"Transfer-Encoding: chunked\r\n";
    if (!WinHttpAddRequestHeaders(
            request.get(),
            kTransferEncodingHeader,
            base::checked_cast<DWORD>(wcslen(kTransferEncodingHeader)),
            WINHTTP_ADDREQ_FLAG_ADD)) {
      LOG(ERROR) << WinHttpMessage("WinHttpAddRequestHeaders");
      return false;
    }

    content_length_dword = WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH;
  } else if (!AssignIfInRange(&content_length_dword, content_length)) {
    content_length_dword = WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH;
  }

  if (!WinHttpSendRequest(request.get(),
                          WINHTTP_NO_ADDITIONAL_HEADERS,
                          0,
                          WINHTTP_NO_REQUEST_DATA,
                          0,
                          content_length_dword,
                          0)) {
    LOG(ERROR) << WinHttpMessage("WinHttpSendRequest");
    return false;
  }

  size_t total_written = 0;
  FileOperationResult data_bytes;
  do {
    struct {
      char size[8];
      char crlf0[2];
      uint8_t data[32 * 1024];
      char crlf1[2];
    } buf;
    static_assert(sizeof(buf) == sizeof(buf.size) +
                                 sizeof(buf.crlf0) +
                                 sizeof(buf.data) +
                                 sizeof(buf.crlf1),
                  "buf should not have padding");

    // Read a block of data.
    data_bytes = body_stream()->GetBytesBuffer(buf.data, sizeof(buf.data));
    if (data_bytes == -1) {
      return false;
    }
    DCHECK_GE(data_bytes, 0);
    DCHECK_LE(static_cast<size_t>(data_bytes), sizeof(buf.data));

    void* write_start;
    DWORD write_size;

    if (chunked) {
      // Chunked encoding uses the entirety of buf. buf.size is presented in
      // hexadecimal without any leading "0x". The terminating CR and LF will be
      // placed immediately following the used portion of buf.data, even if
      // buf.data is not full, and not necessarily in buf.crlf1.

      unsigned int data_bytes_ui = base::checked_cast<unsigned int>(data_bytes);

      // snprintf() would NUL-terminate, but _snprintf() won’t.
      int rv = _snprintf(buf.size, sizeof(buf.size), "%08x", data_bytes_ui);
      DCHECK_GE(rv, 0);
      DCHECK_EQ(static_cast<size_t>(rv), sizeof(buf.size));
      DCHECK_NE(buf.size[sizeof(buf.size) - 1], '\0');

      buf.crlf0[0] = '\r';
      buf.crlf0[1] = '\n';
      buf.data[data_bytes] = '\r';
      buf.data[data_bytes + 1] = '\n';

      // Skip leading zeroes in the chunk size.
      unsigned int size_len;
      for (size_len = sizeof(buf.size); size_len > 1; --size_len) {
        if (buf.size[sizeof(buf.size) - size_len] != '0') {
          break;
        }
      }

      write_start = buf.crlf0 - size_len;
      write_size = base::checked_cast<DWORD>(size_len + sizeof(buf.crlf0) +
                                             data_bytes + sizeof(buf.crlf1));
    } else {
      // When not using chunked encoding, only use buf.data.
      write_start = buf.data;
      write_size = base::checked_cast<DWORD>(data_bytes);
    }

    // write_size will be 0 at EOF in non-chunked mode. Skip the write in that
    // case. In contrast, at EOF in chunked mode, a zero-length chunk must be
    // sent to signal EOF. This will happen when processing the EOF indicated by
    // a 0 return from body_stream()->GetBytesBuffer() above.
    if (write_size != 0) {
      DWORD written;
      if (!WinHttpWriteData(request.get(), write_start, write_size, &written)) {
        LOG(ERROR) << WinHttpMessage("WinHttpWriteData");
        return false;
      }

      DCHECK_EQ(written, write_size);
      total_written += written;
    }
  } while (data_bytes > 0);

  if (!chunked) {
    DCHECK_EQ(total_written, content_length);
  }

  if (!WinHttpReceiveResponse(request.get(), nullptr)) {
    LOG(ERROR) << WinHttpMessage("WinHttpReceiveResponse");
    return false;
  }

  DWORD status_code = 0;
  DWORD sizeof_status_code = sizeof(status_code);

  if (!WinHttpQueryHeaders(
          request.get(),
          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX,
          &status_code,
          &sizeof_status_code,
          WINHTTP_NO_HEADER_INDEX)) {
    LOG(ERROR) << WinHttpMessage("WinHttpQueryHeaders");
    return false;
  }

  if (status_code != 200) {
    LOG(ERROR) << base::StringPrintf("HTTP status %d", status_code);
    return false;
  }

  if (response_body) {
    response_body->clear();

    // There isn’t any reason to call WinHttpQueryDataAvailable(), because it
    // returns the number of bytes available to be read without blocking at the
    // time of the call, not the number of bytes until end-of-file. This method,
    // which executes synchronously, is only concerned with reading until EOF.
    DWORD bytes_read = 0;
    do {
      char read_buffer[4096];
      if (!WinHttpReadData(
              request.get(), read_buffer, sizeof(read_buffer), &bytes_read)) {
        LOG(ERROR) << WinHttpMessage("WinHttpReadData");
        return false;
      }

      response_body->append(read_buffer, bytes_read);
    } while (bytes_read > 0);
  }

  return true;
}

}  // namespace

// static
std::unique_ptr<HTTPTransport> HTTPTransport::Create() {
  return std::unique_ptr<HTTPTransportWin>(new HTTPTransportWin);
}

}  // namespace crashpad
