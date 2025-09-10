//------------------------------------------------------------------------------
// This file is part of XrdHTTP: A pragmatic implementation of the
// HTTP/WebDAV protocol for the Xrootd framework
//
// Copyright (c) 2013 by European Organization for Nuclear Research (CERN)
// Author: Fabrizio Furano <furano@cern.ch>
// File Date: Apr 2013
//------------------------------------------------------------------------------
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//------------------------------------------------------------------------------








/** @file  XrdHttpUtils.cc
 * @brief  Utility functions for XrdHTTP
 * @author Fabrizio Furano
 * @date   April 2013
 * 
 * 
 * 
 */



#include <cstring>
#include <openssl/hmac.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <pthread.h>
#include <memory>
#include <vector>
#include <algorithm>

#include "XProtocol/XPtypes.hh"
#include "XrdSec/XrdSecEntity.hh"
# include "sys/param.h"
#include "XrdOuc/XrdOucString.hh"
#include "XrdHttpUtils.hh"

#if OPENSSL_VERSION_NUMBER < 0x10100000L
static HMAC_CTX* HMAC_CTX_new() {
  HMAC_CTX *ctx = (HMAC_CTX *)OPENSSL_malloc(sizeof(HMAC_CTX));
  if (ctx) HMAC_CTX_init(ctx);
  return ctx;
}

static void HMAC_CTX_free(HMAC_CTX *ctx) {
  if (ctx) {
    HMAC_CTX_cleanup(ctx);
    OPENSSL_free(ctx);
  }
}
#endif


// GetHost from URL
// Parse an URL and extract the host name and port
// Return 0 if OK
int parseURL(char *url, char *host, int &port, char **path) {
  // http://x.y.z.w:p/path

  *path = 0;

  // look for the second slash
  char *p = strstr(url, "//");
  if (!p) return -1;


  p += 2;

  // look for the end of the host:port
  char *p2 = strchr(p, '/');
  if (!p2) return -1;

  *path = p2;

  char buf[256];
  int l = std::min((int)(p2 - p), (int)sizeof (buf));
  strncpy(buf, p, l);
  buf[l] = '\0';

  // Now look for :
  p = strchr(buf, ':');
  if (p) {
    int l = std::min((int)(p - buf), (int)sizeof (buf));
    strncpy(host, buf, l);
    host[l] = '\0';

    port = atoi(p + 1);
  } else {
    port = 0;


    strcpy(host, buf);
  }

  return 0;
}


// Encode an array of bytes to base64

void Tobase64(const unsigned char *input, int length, char *out) {
  BIO *bmem, *b64;
  BUF_MEM *bptr;

  if (!out) return;

  out[0] = '\0';

  b64 = BIO_new(BIO_f_base64());
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  bmem = BIO_new(BIO_s_mem());
  BIO_push(b64, bmem);
  BIO_write(b64, input, length);

  if (BIO_flush(b64) <= 0) {
    BIO_free_all(b64);
    return;
  }

  BIO_get_mem_ptr(b64, &bptr);


  memcpy(out, bptr->data, bptr->length);
  out[bptr->length] = '\0';

  BIO_free_all(b64);

  return;
}


static int
char_to_int(int c)
{
  if (isdigit(c)) {
    return c - '0';
  } else {
    c = tolower(c);
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    return -1;
  }
}


// Decode a hex digest array to raw bytes.
//
bool Fromhexdigest(const unsigned char *input, int length, unsigned char *out) {
  for (int idx=0; idx < length; idx += 2) {
    int upper =  char_to_int(input[idx]);
    int lower =  char_to_int(input[idx+1]);
    if ((upper < 0) || (lower < 0)) {
      return false;
    }
    out[idx/2] = (upper << 4) + lower;
  }
  return true;
}


// Simple itoa function
std::string itos(long i) {
  char buf[128];
  sprintf(buf, "%ld", i);

  return buf;
}



// Home made implementation of strchrnul
char *mystrchrnul(const char *s, int c) {
  char *ptr = strchr((char *)s, c);

  if (!ptr)
    return strchr((char *)s, '\0');

  return ptr;
}



// Calculates the opaque arguments hash, needed for a secure redirection
//
// - hash is a string that will be filled with the hash
//
// - fn: the original filename that was requested
// - dhost: target redirection hostname
// - client: address:port of the client
// - tim: creation time of the url
// - tim_grace: validity time before and after creation time
//
// Input for the key (simple shared secret)
// - key
// - key length
//

void calcHashes(
        char *hash,

        const char *fn,

        kXR_int16 request,

        XrdSecEntity *secent,

        time_t tim,

        const char *key) {


#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  EVP_MAC *mac;
  EVP_MAC_CTX *ctx;
  size_t len;
#else
  HMAC_CTX *ctx;
  unsigned int len;
#endif
  unsigned char mdbuf[EVP_MAX_MD_SIZE];
  char buf[64];
  struct tm tms;


  if (!hash) {
    return;
  }
  hash[0] = '\0';
  
  if (!key) {
    return;
  }

  if (!fn || !secent) {
    return;
  }

#if OPENSSL_VERSION_NUMBER >= 0x30000000L

  mac = EVP_MAC_fetch(0, "sha256", 0);
  ctx = EVP_MAC_CTX_new(mac);

  if (!ctx) {
     return;
  }


  EVP_MAC_init(ctx, (const unsigned char *) key, strlen(key), 0);


  if (fn)
    EVP_MAC_update(ctx, (const unsigned char *) fn,
          strlen(fn) + 1);

  EVP_MAC_update(ctx, (const unsigned char *) &request,
          sizeof (request));

  if (secent->name)
    EVP_MAC_update(ctx, (const unsigned char *) secent->name,
          strlen(secent->name) + 1);

  if (secent->vorg)
    EVP_MAC_update(ctx, (const unsigned char *) secent->vorg,
          strlen(secent->vorg) + 1);

  if (secent->host)
    EVP_MAC_update(ctx, (const unsigned char *) secent->host,
          strlen(secent->host) + 1);

  if (secent->moninfo)
    EVP_MAC_update(ctx, (const unsigned char *) secent->moninfo,
          strlen(secent->moninfo) + 1);

  localtime_r(&tim, &tms);
  strftime(buf, sizeof (buf), "%s", &tms);
  EVP_MAC_update(ctx, (const unsigned char *) buf,
          strlen(buf) + 1);

  EVP_MAC_final(ctx, mdbuf, &len, EVP_MAX_MD_SIZE);

  EVP_MAC_CTX_free(ctx);
  EVP_MAC_free(mac);

#else

  ctx = HMAC_CTX_new();

  if (!ctx) {
    return;
  }



  HMAC_Init_ex(ctx, (const void *) key, strlen(key), EVP_sha256(), 0);


  if (fn)
    HMAC_Update(ctx, (const unsigned char *) fn,
          strlen(fn) + 1);

  HMAC_Update(ctx, (const unsigned char *) &request,
          sizeof (request));

  if (secent->name)
    HMAC_Update(ctx, (const unsigned char *) secent->name,
          strlen(secent->name) + 1);

  if (secent->vorg)
    HMAC_Update(ctx, (const unsigned char *) secent->vorg,
          strlen(secent->vorg) + 1);

  if (secent->host)
    HMAC_Update(ctx, (const unsigned char *) secent->host,
          strlen(secent->host) + 1);

  if (secent->moninfo)
    HMAC_Update(ctx, (const unsigned char *) secent->moninfo,
          strlen(secent->moninfo) + 1);

  localtime_r(&tim, &tms);
  strftime(buf, sizeof (buf), "%s", &tms);
  HMAC_Update(ctx, (const unsigned char *) buf,
          strlen(buf) + 1);

  HMAC_Final(ctx, mdbuf, &len);

  HMAC_CTX_free(ctx);

#endif

  Tobase64(mdbuf, len / 2, hash);
}

int compareHash(
        const char *h1,
        const char *h2) {

  if (h1 == h2) return 0;

  if (!h1 || !h2)
    return 1;

  return strcmp(h1, h2);

}

// unquote a string and return a new one

char *unquote(char *str) {
  int l = strlen(str);
  char *r = (char *) malloc(l + 1);
  r[0] = '\0';
  int i, j = 0;

  for (i = 0; i < l; i++) {

    if (str[i] == '%') {
      char savec = str[i + 3];
      str[i + 3] = '\0';

      r[j] = strtol(str + i + 1, 0, 16);
      str[i + 3] = savec;

      i += 2;
    } else r[j] = str[i];

    j++;
  }

  r[j] = '\0';

  return r;

}

// Quote a string and return a new one

char *quote(const char *str) {
  int l = strlen(str);
  char *r = (char *) malloc(l*3 + 1);
  r[0] = '\0';
  int i, j = 0;

  for (i = 0; i < l; i++) {
    char c = str[i];

    switch (c) {
      case ' ':
        strcpy(r + j, "%20");
        j += 3;
        break;
      case '[':
        strcpy(r + j, "%5B");
        j += 3;
        break;
      case ']':
        strcpy(r + j, "%5D");
        j += 3;
        break;
      case ':':
        strcpy(r + j, "%3A");
        j += 3;
        break;
      // case '/':
      //   strcpy(r + j, "%2F");
      //   j += 3;
      //   break;
      case '#':
         strcpy(r + j, "%23");
         j += 3;
         break;
      case '\n':
        strcpy(r + j, "%0A");
        j += 3;
        break;
      case '\r':
        strcpy(r + j, "%0D");
        j += 3;
        break;
      case '=':
        strcpy(r + j, "%3D");
        j += 3;
        break;
      default:
        r[j++] = c;
    }
  }

  r[j] = '\0';

  return r;
}


// Escape a string and return a new one

char *escapeXML(const char *str) {
  int l = strlen(str);
  char *r = (char *) malloc(l*6 + 1);
  r[0] = '\0';
  int i, j = 0;
  
  for (i = 0; i < l; i++) {
    char c = str[i];
    
    switch (c) {
      case '"':
        strcpy(r + j, "&quot;");
        j += 6;
        break;
      case '&':
        strcpy(r + j, "&amp;");
        j += 5;
        break;
      case '<':
        strcpy(r + j, "&lt;");
        j += 4;
        break;
      case '>':
        strcpy(r + j, "&gt;");
        j += 4;
        break;
      case '\'':
        strcpy(r + j, "&apos;");
        j += 6;
        break;
      
      default:
        r[j++] = c;
    }
  }
  
  r[j] = '\0';
  
  return r;
}

int mapXrdErrToHttp(XErrorCode xrdError) {

  HttpStatus statusCode;

  switch(xrdError){
    case kXR_AuthFailed:
      statusCode = HTTP_FORBIDDEN; break;

    case kXR_NotAuthorized:
      statusCode = HTTP_UNAUTHORIZED; break;

    case kXR_NotFound:
      statusCode = HTTP_NOT_FOUND; break;

    case kXR_Unsupported:
    case kXR_InvalidRequest:  // NOT_ACCEPTABLE?
      statusCode = HTTP_METHOD_NOT_ALLOWED; break;

    case kXR_FileLocked:
      statusCode = HTTP_LOCKED; break;

    case kXR_isDirectory:  // UNPROCESSABLE_ENTITY?
    case kXR_ItExists:
    case kXR_Conflict:
      statusCode = HTTP_CONFLICT; break;

    case kXR_noserver:
      statusCode = HTTP_BAD_GATEWAY; break;

    case kXR_TimerExpired:
      statusCode = HTTP_GATEWAY_TIMEOUT; break;

    case kXR_NoSpace:
    case kXR_overQuota:
      statusCode = HTTP_INSUFFICIENT_STORAGE; break;

    default:
      statusCode = HTTP_IM_A_TEAPOT;
    }

  return static_cast<int>(statusCode);
}

int mapErrNoToHttp(int err) {
  HttpStatus statusCode;

  switch (err) {
    case EACCES:
    case EPERM:
      statusCode = HTTP_FORBIDDEN;
      break;

    case ENOENT:
      statusCode = HTTP_NOT_FOUND;
      break;

    case EEXIST:
      statusCode = HTTP_CONFLICT;
      break;

    case ENOTDIR:
    case EISDIR:
      statusCode = HTTP_UNPROCESSABLE_ENTITY;
      break;

    case ENOSPC:
    case EDQUOT:
      statusCode = HTTP_INSUFFICIENT_STORAGE;
      break;

    case EIO:
      statusCode = HTTP_INTERNAL_SERVER_ERROR;
      break;

    case EINVAL:
    case ENAMETOOLONG:
      statusCode = HTTP_BAD_REQUEST;
      break;

    case ENOTSUP:
      statusCode = HTTP_NOT_IMPLEMENTED;
      break;

    case EMFILE:
    case ENFILE:
      statusCode = HTTP_SERVICE_UNAVAILABLE;
      break;

    case EBUSY:
    case EAGAIN:
    case ETIMEDOUT:
      statusCode = HTTP_SERVICE_UNAVAILABLE;
      break;

    case ECONNREFUSED:
    case ECONNRESET:
    case ENETDOWN:
    case ENETUNREACH:
    case EHOSTUNREACH:
      statusCode = HTTP_BAD_GATEWAY;
      break;

    default:
      statusCode = HTTP_INTERNAL_SERVER_ERROR;
  }

  return static_cast<int>(statusCode);
}


std::string httpStatusToString(HttpStatus status) {
  switch (status) {
    // 1xx Informational
    case HTTP_CONTINUE:
      return "Continue";
    case HTTP_SWITCHING_PROTOCOLS:
      return "Switching Protocols";
    case HTTP_PROCESSING:
      return "Processing";
    case HTTP_EARLY_HINTS:
      return "Early Hints";

    // 2xx Success
    case HTTP_OK:
      return "OK";
    case HTTP_CREATED:
      return "Created";
    case HTTP_ACCEPTED:
      return "Accepted";
    case HTTP_NON_AUTHORITATIVE_INFORMATION:
      return "Non-Authoritative Information";
    case HTTP_NO_CONTENT:
      return "No Content";
    case HTTP_RESET_CONTENT:
      return "Reset Content";
    case HTTP_PARTIAL_CONTENT:
      return "Partial Content";
    case HTTP_MULTI_STATUS:
      return "Multi-Status";
    case HTTP_ALREADY_REPORTED:
      return "Already Reported";
    case HTTP_IM_USED:
      return "IM Used";

    // 3xx Redirection
    case HTTP_MULTIPLE_CHOICES:
      return "Multiple Choices";
    case HTTP_MOVED_PERMANENTLY:
      return "Moved Permanently";
    case HTTP_FOUND:
      return "Found";
    case HTTP_SEE_OTHER:
      return "See Other";
    case HTTP_NOT_MODIFIED:
      return "Not Modified";
    case HTTP_USE_PROXY:
      return "Use Proxy";
    case HTTP_TEMPORARY_REDIRECT:
      return "Temporary Redirect";
    case HTTP_PERMANENT_REDIRECT:
      return "Permanent Redirect";

    // 4xx Client Errors
    case HTTP_BAD_REQUEST:
      return "Bad Request";
    case HTTP_UNAUTHORIZED:
      return "Unauthorized";
    case HTTP_PAYMENT_REQUIRED:
      return "Payment Required";
    case HTTP_FORBIDDEN:
      return "Forbidden";
    case HTTP_NOT_FOUND:
      return "Not Found";
    case HTTP_METHOD_NOT_ALLOWED:
      return "Method Not Allowed";
    case HTTP_NOT_ACCEPTABLE:
      return "Not Acceptable";
    case HTTP_PROXY_AUTHENTICATION_REQUIRED:
      return "Proxy Authentication Required";
    case HTTP_REQUEST_TIMEOUT:
      return "Request Timeout";
    case HTTP_CONFLICT:
      return "Conflict";
    case HTTP_GONE:
      return "Gone";
    case HTTP_LENGTH_REQUIRED:
      return "Length Required";
    case HTTP_PRECONDITION_FAILED:
      return "Precondition Failed";
    case HTTP_PAYLOAD_TOO_LARGE:
      return "Payload Too Large";
    case HTTP_URI_TOO_LONG:
      return "URI Too Long";
    case HTTP_UNSUPPORTED_MEDIA_TYPE:
      return "Unsupported Media Type";
    case HTTP_RANGE_NOT_SATISFIABLE:
      return "Range Not Satisfiable";
    case HTTP_EXPECTATION_FAILED:
      return "Expectation Failed";
    case HTTP_IM_A_TEAPOT:
      return "I'm a teapot";
    case HTTP_MISDIRECTED_REQUEST:
      return "Misdirected Request";
    case HTTP_UNPROCESSABLE_ENTITY:
      return "Unprocessable Entity";
    case HTTP_LOCKED:
      return "Locked";
    case HTTP_FAILED_DEPENDENCY:
      return "Failed Dependency";
    case HTTP_TOO_EARLY:
      return "Too Early";
    case HTTP_UPGRADE_REQUIRED:
      return "Upgrade Required";
    case HTTP_PRECONDITION_REQUIRED:
      return "Precondition Required";
    case HTTP_TOO_MANY_REQUESTS:
      return "Too Many Requests";
    case HTTP_REQUEST_HEADER_FIELDS_TOO_LARGE:
      return "Request Header Fields Too Large";
    case HTTP_UNAVAILABLE_FOR_LEGAL_REASONS:
      return "Unavailable For Legal Reasons";

    // 5xx Server Errors
    case HTTP_INTERNAL_SERVER_ERROR:
      return "Internal Server Error";
    case HTTP_NOT_IMPLEMENTED:
      return "Not Implemented";
    case HTTP_BAD_GATEWAY:
      return "Bad Gateway";
    case HTTP_SERVICE_UNAVAILABLE:
      return "Service Unavailable";
    case HTTP_GATEWAY_TIMEOUT:
      return "Gateway Timeout";
    case HTTP_HTTP_VERSION_NOT_SUPPORTED:
      return "HTTP Version Not Supported";
    case HTTP_VARIANT_ALSO_NEGOTIATES:
      return "Variant Also Negotiates";
    case HTTP_INSUFFICIENT_STORAGE:
      return "Insufficient Storage";
    case HTTP_LOOP_DETECTED:
      return "Loop Detected";
    case HTTP_NOT_EXTENDED:
      return "Not Extended";
    case HTTP_NETWORK_AUTHENTICATION_REQUIRED:
      return "Network Authentication Required";
    default:
      return "UNKNOWN";
  }
}
