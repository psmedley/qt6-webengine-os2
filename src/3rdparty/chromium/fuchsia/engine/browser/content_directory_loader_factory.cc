// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fuchsia/engine/browser/content_directory_loader_factory.h"

#include <lib/fdio/directory.h>
#include <lib/fdio/fdio.h>

#include <algorithm>
#include <memory>
#include <utility>

#include "base/command_line.h"
#include "base/files/memory_mapped_file.h"
#include "base/fuchsia/fuchsia_logging.h"
#include "base/json/json_reader.h"
#include "base/lazy_instance.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_piece.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/task/post_task.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "fuchsia/base/fuchsia_dir_scheme.h"
#include "fuchsia/engine/common/web_engine_content_client.h"
#include "fuchsia/engine/switches.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe_producer.h"
#include "mojo/public/cpp/system/string_data_source.h"
#include "net/base/filename_util.h"
#include "net/base/mime_sniffer.h"
#include "net/base/parse_number.h"
#include "net/http/http_byte_range.h"
#include "net/http/http_util.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace {

// Maximum number of bytes to read when "sniffing" its MIME type.
constexpr size_t kMaxBytesToSniff = 1024 * 10;  // Read up to 10KB.

// The MIME type to use if "sniffing" fails to compute a result.
constexpr char kFallbackMimeType[] = "application/octet-stream";

// Returns a list of populated response HTTP headers.
// |mime_type|: The MIME type of the resource.
// |charset|: The resource's character set. Optional. If omitted, the browser
//            will assume the charset to be "text/plain" by default.
scoped_refptr<net::HttpResponseHeaders> CreateHeaders(
    base::StringPiece mime_type,
    const absl::optional<std::string>& charset) {
  constexpr char kXFrameOptions[] = "X-Frame-Options";
  constexpr char kXFrameOptionsValue[] = "DENY";
  constexpr char kCacheControl[] = "Cache-Control";
  constexpr char kCacheControlValue[] = "no-cache";
  constexpr char kContentType[] = "Content-Type";
  constexpr char kCharsetSeparator[] = "; charset=";

  auto headers =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/1.1 200 OK\r\n");
  headers->SetHeader(kXFrameOptions, kXFrameOptionsValue);
  headers->SetHeader(kCacheControl, kCacheControlValue);

  if (charset) {
    headers->SetHeader(kContentType,
                       base::StrCat({mime_type, kCharsetSeparator, *charset}));
  } else {
    headers->SetHeader(kContentType, mime_type);
  }

  return headers;
}

// Determines which range of bytes should be sent in the response.
// If a range is specified in |headers|, then |start| and |length| are set to
// the range's boundaries and the function returns true.
// If no range is specified in |headers|, then the entire range [0,
// |max_length|) is set and the function returns true.
// If the requested range is invalid, then the function returns false.
bool GetRangeForRequest(const net::HttpRequestHeaders& headers,
                        size_t max_length,
                        size_t* start,
                        size_t* length) {
  std::string range_header;
  net::HttpByteRange byte_range;
  if (headers.GetHeader(net::HttpRequestHeaders::kRange, &range_header)) {
    std::vector<net::HttpByteRange> ranges;
    if (net::HttpUtil::ParseRangeHeader(range_header, &ranges) &&
        ranges.size() == 1) {
      byte_range = ranges[0];
    } else {
      // Only one range is allowed.
      return false;
    }
  }
  if (!byte_range.ComputeBounds(max_length)) {
    return false;
  }

  *start = byte_range.first_byte_position();
  *length =
      byte_range.last_byte_position() - byte_range.first_byte_position() + 1;
  return true;
}

// Copies data from a fuchsia.io.Node file into a URL response stream.
class ContentDirectoryURLLoader final : public network::mojom::URLLoader {
 public:
  ContentDirectoryURLLoader() = default;
  ~ContentDirectoryURLLoader() override = default;

  ContentDirectoryURLLoader(const ContentDirectoryURLLoader&) = delete;
  ContentDirectoryURLLoader& operator=(const ContentDirectoryURLLoader&) =
      delete;

  // Creates a read-only MemoryMappedFile view to |file|.
  bool MapFile(fidl::InterfaceHandle<fuchsia::io::Node> file,
               base::MemoryMappedFile* mmap) {
    // Bind the file channel to a FDIO entry and then a file descriptor so that
    // we can use it for reading.
    fdio_t* fdio = nullptr;
    zx_status_t status = fdio_create(file.TakeChannel().release(), &fdio);
    if (status == ZX_ERR_PEER_CLOSED) {
      // File-not-found errors are expected in some cases, so handle this result
      // w/o logging error text.
      return false;
    } else if (status != ZX_OK) {
      ZX_DLOG_IF(WARNING, status != ZX_OK, status) << "fdio_create";
      return false;
    }

    base::ScopedFD fd(fdio_bind_to_fd(fdio, -1, 0));
    if (!fd.is_valid()) {
      LOG(ERROR) << "fdio_bind_to_fd returned an invalid FD.";
      return false;
    }

    // Map the file into memory.
    if (!mmap->Initialize(base::File(std::move(fd)),
                          base::MemoryMappedFile::READ_ONLY)) {
      return false;
    }

    return true;
  }

  // Initiates data transfer from |file_channel| to |client_remote|.
  // |metadata_channel|, if it is connected to a file, is accessed to get the
  // MIME type and charset of the file.
  static void CreateAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> url_loader_receiver,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client_remote,
      fidl::InterfaceHandle<fuchsia::io::Node> file_channel,
      fidl::InterfaceHandle<fuchsia::io::Node> metadata_channel) {
    std::unique_ptr<ContentDirectoryURLLoader> loader =
        std::make_unique<ContentDirectoryURLLoader>();
    loader->Start(request, std::move(client_remote), std::move(file_channel),
                  std::move(metadata_channel));

    // |loader|'s lifetime is bound to the lifetime of the URLLoader Mojo
    // client endpoint.
    mojo::MakeSelfOwnedReceiver(std::move(loader),
                                std::move(url_loader_receiver));
  }

  void Start(const network::ResourceRequest& request,
             mojo::PendingRemote<network::mojom::URLLoaderClient> client_remote,
             fidl::InterfaceHandle<fuchsia::io::Node> file_channel,
             fidl::InterfaceHandle<fuchsia::io::Node> metadata_channel) {
    client_.Bind(std::move(client_remote));

    if (!MapFile(std::move(file_channel), &mmap_)) {
      client_->OnComplete(network::URLLoaderCompletionStatus(net::ERR_FAILED));
      return;
    }

    // Construct and deliver the HTTP response header.
    auto response = network::mojom::URLResponseHead::New();

    // Read the charset and MIME type from the optional _metadata file.
    absl::optional<std::string> charset;
    absl::optional<std::string> mime_type;
    base::MemoryMappedFile metadata_mmap;
    if (MapFile(std::move(metadata_channel), &metadata_mmap)) {
      absl::optional<base::Value> metadata_parsed = base::JSONReader::Read(
          base::StringPiece(reinterpret_cast<char*>(metadata_mmap.data()),
                            metadata_mmap.length()));

      if (metadata_parsed && metadata_parsed->is_dict()) {
        if (metadata_parsed->FindStringKey("charset"))
          charset = *metadata_parsed->FindStringKey("charset");

        if (metadata_parsed->FindStringKey("mime"))
          mime_type = *metadata_parsed->FindStringKey("mime");
      }
    }

    // If a MIME type wasn't specified, then fall back on inferring the type
    // from the file's contents.
    if (!mime_type) {
      if (!net::SniffMimeType(
              base::StringPiece(reinterpret_cast<char*>(mmap_.data()),
                                std::min(mmap_.length(), kMaxBytesToSniff)),
              request.url, {} /* type_hint */,
              net::ForceSniffFileUrlsForHtml::kDisabled,
              &mime_type.emplace())) {
        if (!mime_type) {
          // Only set the fallback type if SniffMimeType completely gave up on
          // generating a suggestion.
          *mime_type = kFallbackMimeType;
        }
      }
    }

    size_t start_offset;
    size_t content_length;
    if (!GetRangeForRequest(request.headers, mmap_.length(), &start_offset,
                            &content_length)) {
      client_->OnComplete(network::URLLoaderCompletionStatus(
          net::ERR_REQUEST_RANGE_NOT_SATISFIABLE));
      return;
    }

    response->mime_type = *mime_type;
    response->headers = CreateHeaders(*mime_type, charset);
    response->content_length = content_length;
    client_->OnReceiveResponse(std::move(response));

    // Set up the Mojo DataPipe used for streaming the response payload to the
    // client.
    mojo::ScopedDataPipeProducerHandle producer_handle;
    mojo::ScopedDataPipeConsumerHandle consumer_handle;
    MojoResult rv = mojo::CreateDataPipe(0u, producer_handle, consumer_handle);
    if (rv != MOJO_RESULT_OK) {
      client_->OnComplete(
          network::URLLoaderCompletionStatus(net::ERR_INSUFFICIENT_RESOURCES));
      return;
    }

    client_->OnStartLoadingResponseBody(std::move(consumer_handle));

    // Start reading the contents of |mmap_| into the response DataPipe.
    body_writer_ =
        std::make_unique<mojo::DataPipeProducer>(std::move(producer_handle));
    body_writer_->Write(
        std::make_unique<mojo::StringDataSource>(
            base::StringPiece(
                reinterpret_cast<char*>(mmap_.data() + start_offset),
                content_length),
            mojo::StringDataSource::AsyncWritingMode::
                STRING_STAYS_VALID_UNTIL_COMPLETION),
        base::BindOnce(&ContentDirectoryURLLoader::OnWriteComplete,
                       base::Unretained(this)));
  }

  // network::mojom::URLLoader implementation:
  void FollowRedirect(
      const std::vector<std::string>& removed_headers,
      const net::HttpRequestHeaders& modified_request_headers,
      const net::HttpRequestHeaders& modified_cors_exempt_request_headers,
      const absl::optional<GURL>& new_url) override {}
  void SetPriority(net::RequestPriority priority,
                   int32_t intra_priority_value) override {}
  void PauseReadingBodyFromNet() override {}
  void ResumeReadingBodyFromNet() override {}

 private:
  // Called when body_writer_->Write() has completed asynchronously.
  void OnWriteComplete(MojoResult result) {
    // Signal stream EOF to the client.
    body_writer_.reset();

    if (result != MOJO_RESULT_OK) {
      client_->OnComplete(network::URLLoaderCompletionStatus(net::ERR_FAILED));
      return;
    }

    network::URLLoaderCompletionStatus status(net::OK);
    status.encoded_data_length = mmap_.length();
    status.encoded_body_length = mmap_.length();
    status.decoded_body_length = mmap_.length();
    client_->OnComplete(std::move(status));
  }

  // Used for sending status codes and response payloads to the client.
  mojo::Remote<network::mojom::URLLoaderClient> client_;

  // A read-only, memory mapped view of the file being loaded.
  base::MemoryMappedFile mmap_;

  // Manages chunked data transfer over the response DataPipe.
  std::unique_ptr<mojo::DataPipeProducer> body_writer_;
};

net::Error OpenFileFromDirectory(
    const std::string& content_directory_name,
    const base::FilePath& relative_file_path,
    fidl::InterfaceRequest<fuchsia::io::Node> file_request) {
  DCHECK(file_request);
  DCHECK(!relative_file_path.IsAbsolute());

  auto absolute_file_path =
      base::FilePath(ContentDirectoryLoaderFactory::kContentDirectoriesPath)
          .Append(content_directory_name)
          .Append(relative_file_path);

  zx_status_t status = fdio_open(absolute_file_path.value().c_str(),
                                 fuchsia::io::OPEN_RIGHT_READABLE,
                                 file_request.TakeChannel().release());
  if (status != ZX_OK) {
    ZX_DLOG(WARNING, status) << "fdio_open";
    return net::ERR_FILE_NOT_FOUND;
  }

  return net::OK;
}

}  // namespace

// static
const char ContentDirectoryLoaderFactory::kContentDirectoriesPath[] =
    "/content-directories";

// static
mojo::PendingRemote<network::mojom::URLLoaderFactory>
ContentDirectoryLoaderFactory::Create() {
  mojo::PendingRemote<network::mojom::URLLoaderFactory> pending_remote;

  // The ContentDirectoryLoaderFactory will delete itself when there are no more
  // receivers - see the network::SelfDeletingURLLoaderFactory::OnDisconnect
  // method.
  new ContentDirectoryLoaderFactory(
      pending_remote.InitWithNewPipeAndPassReceiver());

  return pending_remote;
}

ContentDirectoryLoaderFactory::ContentDirectoryLoaderFactory(
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> factory_receiver)
    : network::SelfDeletingURLLoaderFactory(std::move(factory_receiver)),
      task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN})) {}

ContentDirectoryLoaderFactory::~ContentDirectoryLoaderFactory() = default;

void ContentDirectoryLoaderFactory::CreateLoaderAndStart(
    mojo::PendingReceiver<network::mojom::URLLoader> loader,
    int32_t request_id,
    uint32_t options,
    const network::ResourceRequest& request,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client,
    const net::MutableNetworkTrafficAnnotationTag& traffic_annotation) {
  if (!request.url.SchemeIs(cr_fuchsia::kFuchsiaDirScheme) ||
      !request.url.is_valid()) {
    mojo::Remote<network::mojom::URLLoaderClient>(std::move(client))
        ->OnComplete(network::URLLoaderCompletionStatus(net::ERR_INVALID_URL));
    return;
  }

  if (request.method != "GET") {
    mojo::Remote<network::mojom::URLLoaderClient>(std::move(client))
        ->OnComplete(
            network::URLLoaderCompletionStatus(net::ERR_METHOD_NOT_SUPPORTED));
    return;
  }

  // Fuchsia paths do not support the notion of absolute paths, so strip the
  // leading slash from the URL's path fragment.
  base::StringPiece requested_path = request.url.path_piece();
  DCHECK(base::StartsWith(requested_path, "/"));
  requested_path.remove_prefix(1);

  fidl::InterfaceHandle<fuchsia::io::Node> file_handle;
  net::Error open_result = OpenFileFromDirectory(request.url.GetOrigin().host(),
                                                 base::FilePath(requested_path),
                                                 file_handle.NewRequest());
  if (open_result != net::OK) {
    mojo::Remote<network::mojom::URLLoaderClient>(std::move(client))
        ->OnComplete(network::URLLoaderCompletionStatus(open_result));
    return;
  }
  DCHECK(file_handle);

  // Metadata files are optional. In the event that the file is absent,
  // |metadata_channel| will produce a ZX_CHANNEL_PEER_CLOSED status inside
  // ContentDirectoryURLLoader::Start().
  fidl::InterfaceHandle<fuchsia::io::Node> metadata_handle;
  open_result = OpenFileFromDirectory(
      request.url.GetOrigin().host(),
      base::FilePath(base::StrCat({requested_path, "._metadata"})),
      metadata_handle.NewRequest());
  if (open_result != net::OK) {
    mojo::Remote<network::mojom::URLLoaderClient>(std::move(client))
        ->OnComplete(network::URLLoaderCompletionStatus(open_result));
    return;
  }

  // Load the resource on a blocking-capable TaskRunner.
  task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&ContentDirectoryURLLoader::CreateAndStart,
                     std::move(loader), request, std::move(client),
                     std::move(file_handle), std::move(metadata_handle)));
}
