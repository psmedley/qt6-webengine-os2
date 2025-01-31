// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/file_system_access/file_system_access_manager_impl.h"

#include <algorithm>
#include <string>

#include "base/bind.h"
#include "base/bind_post_task.h"
#include "base/callback_helpers.h"
#include "base/check_op.h"
#include "base/command_line.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/metrics/histogram_functions.h"
#include "base/notreached.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "content/browser/file_system_access/file_system_access.pb.h"
#include "content/browser/file_system_access/file_system_access_access_handle_host_impl.h"
#include "content/browser/file_system_access/file_system_access_data_transfer_token_impl.h"
#include "content/browser/file_system_access/file_system_access_directory_handle_impl.h"
#include "content/browser/file_system_access/file_system_access_error.h"
#include "content/browser/file_system_access/file_system_access_file_handle_impl.h"
#include "content/browser/file_system_access/file_system_access_file_writer_impl.h"
#include "content/browser/file_system_access/file_system_access_transfer_token_impl.h"
#include "content/browser/file_system_access/file_system_chooser.h"
#include "content/browser/file_system_access/fixed_file_system_access_permission_grant.h"
#include "content/browser/storage_partition_impl.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_client.h"
#include "content/public/common/content_switches.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "net/base/escape.h"
#include "net/base/filename_util.h"
#include "services/network/public/cpp/is_potentially_trustworthy.h"
#include "storage/browser/file_system/file_system_context.h"
#include "storage/browser/file_system/file_system_operation_runner.h"
#include "storage/browser/file_system/file_system_url.h"
#include "storage/common/file_system/file_system_types.h"
#include "storage/common/file_system/file_system_util.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_data_transfer_token.mojom.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_error.mojom.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_manager.mojom-forward.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace content {

using blink::mojom::FileSystemAccessStatus;
using PermissionStatus = FileSystemAccessPermissionGrant::PermissionStatus;
using SensitiveDirectoryResult =
    FileSystemAccessPermissionContext::SensitiveDirectoryResult;
using storage::FileSystemContext;
using HandleType = FileSystemAccessPermissionContext::HandleType;
using PathInfo = FileSystemAccessPermissionContext::PathInfo;

namespace {

void ShowFilePickerOnUIThread(const url::Origin& requesting_origin,
                              GlobalRenderFrameHostId frame_id,
                              const FileSystemChooser::Options& options,
                              FileSystemChooser::ResultCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  RenderFrameHost* rfh = RenderFrameHost::FromID(frame_id);
  WebContents* web_contents = WebContents::FromRenderFrameHost(rfh);

  if (!web_contents) {
    std::move(callback).Run(file_system_access_error::FromStatus(
                                FileSystemAccessStatus::kOperationAborted),
                            {});
    return;
  }

  url::Origin embedding_origin =
      url::Origin::Create(web_contents->GetLastCommittedURL());
  if (embedding_origin != requesting_origin) {
    // Third party iframes are not allowed to show a file picker.
    std::move(callback).Run(
        file_system_access_error::FromStatus(
            FileSystemAccessStatus::kPermissionDenied,
            "Third party iframes are not allowed to show a file picker."),
        {});
    return;
  }

  // Drop fullscreen mode so that the user sees the URL bar.
  base::ScopedClosureRunner fullscreen_block =
      web_contents->ForSecurityDropFullscreen();

  FileSystemChooser::CreateAndShow(web_contents, options, std::move(callback),
                                   std::move(fullscreen_block));
}

// Called after creating a file that was picked by a save file picker. If
// creation succeeded (or the file already existed) this will attempt to
// truncate the file to zero bytes, and call `callback` on `reply_runner`
// with the result of this operation.
void DidCreateFileToTruncate(
    storage::FileSystemURL url,
    base::OnceCallback<void(bool)> callback,
    scoped_refptr<base::SequencedTaskRunner> reply_runner,
    storage::FileSystemOperationRunner* operation_runner,
    base::File::Error result) {
  if (result != base::File::FILE_OK) {
    // Failed to create the file, don't even try to truncate it.
    reply_runner->PostTask(FROM_HERE,
                           base::BindOnce(std::move(callback), false));
    return;
  }
  operation_runner->Truncate(
      url, /*length=*/0,
      base::BindOnce(
          [](base::OnceCallback<void(bool)> callback,
             scoped_refptr<base::SequencedTaskRunner> reply_runner,
             base::File::Error result) {
            reply_runner->PostTask(
                FROM_HERE, base::BindOnce(std::move(callback),
                                          result == base::File::FILE_OK));
          },
          std::move(callback), std::move(reply_runner)));
}

// Creates and truncates the file at `url`. Calls `callback` on `reply_runner`
// with true if this succeeded, or false if either creation or truncation
// failed.
void CreateAndTruncateFile(
    storage::FileSystemURL url,
    base::OnceCallback<void(bool)> callback,
    scoped_refptr<base::SequencedTaskRunner> reply_runner,
    storage::FileSystemOperationRunner* operation_runner) {
  // Binding operation_runner as a raw pointer is safe, since the callback is
  // invoked by the operation runner, and thus won't be invoked if the operation
  // runner has been destroyed.
  operation_runner->CreateFile(
      url, /*exclusive=*/false,
      base::BindOnce(&DidCreateFileToTruncate, url, std::move(callback),
                     std::move(reply_runner), operation_runner));
}

bool IsValidTransferToken(FileSystemAccessTransferTokenImpl* token,
                          const url::Origin& expected_origin,
                          HandleType expected_handle_type) {
  if (!token) {
    return false;
  }

  if (token->type() != expected_handle_type) {
    return false;
  }

  if (token->origin() != expected_origin) {
    return false;
  }

  return true;
}

HandleType HandleTypeFromFileInfo(base::File::Error result,
                                  const base::File::Info& file_info) {
  // If we couldn't determine if the url is a directory, it is treated
  // as a file. If the web-exposed API is ever changed to allow
  // reporting errors when getting a dropped file as a
  // FileSystemHandle, this would be one place such errors could be
  // triggered.
  if (result == base::File::FILE_OK && file_info.is_directory) {
    return HandleType::kDirectory;
  }
  return HandleType::kFile;
}

void HandleTransferTokenAsDefaultDirectory(
    FileSystemAccessTransferTokenImpl* token,
    PathInfo& info) {
  auto token_url_type = token->url().type();
  auto token_url_mount_type = token->url().mount_type();

  // Ignore sandboxed file system URLs.
  if (token_url_type == storage::kFileSystemTypeTemporary ||
      token_url_type == storage::kFileSystemTypePersistent) {
    return;
  }

  if (token_url_mount_type == storage::kFileSystemTypeExternal) {
    info.type = FileSystemAccessPermissionContext::PathType::kExternal;
    info.path = token->type() == HandleType::kFile
                    ? token->url().virtual_path().DirName()
                    : token->url().virtual_path();
    return;
  }

  DCHECK(token_url_type == storage::kFileSystemTypeLocal);
  info.path = token->type() == HandleType::kFile ? token->url().path().DirName()
                                                 : token->url().path();
}

bool IsValidIdChar(const char c) {
  return base::IsAsciiAlpha(c) || base::IsAsciiDigit(c) || c == '_' || c == '-';
}

bool IsValidId(const std::string& id) {
  return id.size() <= 32 &&
         std::find_if(id.begin(), id.end(), [](const char c) {
           return !IsValidIdChar(c);
         }) == id.end();
}

ui::SelectFileDialog::Type GetSelectFileDialogType(
    blink::mojom::FilePickerOptionsPtr& options) {
  switch (options->which()) {
    case blink::mojom::FilePickerOptions::Tag::kOpenFilePickerOptions:
      return options->get_open_file_picker_options()->can_select_multiple_files
                 ? ui::SelectFileDialog::SELECT_OPEN_MULTI_FILE
                 : ui::SelectFileDialog::SELECT_OPEN_FILE;
    case blink::mojom::FilePickerOptions::Tag::kSaveFilePickerOptions:
      return ui::SelectFileDialog::SELECT_SAVEAS_FILE;
    case blink::mojom::FilePickerOptions::Tag::kDirectoryPickerOptions:
      return ui::SelectFileDialog::SELECT_FOLDER;
  }
  NOTREACHED();
  return ui::SelectFileDialog::SELECT_NONE;
}

blink::mojom::AcceptsTypesInfoPtr GetAndMoveAcceptsTypesInfo(
    blink::mojom::FilePickerOptionsPtr& options) {
  switch (options->which()) {
    case blink::mojom::FilePickerOptions::Tag::kOpenFilePickerOptions:
      return std::move(
          options->get_open_file_picker_options()->accepts_types_info);
    case blink::mojom::FilePickerOptions::Tag::kSaveFilePickerOptions:
      return std::move(
          options->get_save_file_picker_options()->accepts_types_info);
    case blink::mojom::FilePickerOptions::Tag::kDirectoryPickerOptions:
      return blink::mojom::AcceptsTypesInfo::New(
          /*accepts=*/std::vector<
              blink::mojom::ChooseFileSystemEntryAcceptsOptionPtr>(),
          /*include_accepts_all=*/false);
  }
}

}  // namespace

FileSystemAccessManagerImpl::SharedHandleState::SharedHandleState(
    scoped_refptr<FileSystemAccessPermissionGrant> read_grant,
    scoped_refptr<FileSystemAccessPermissionGrant> write_grant)
    : read_grant(std::move(read_grant)), write_grant(std::move(write_grant)) {
  DCHECK(this->read_grant);
  DCHECK(this->write_grant);
}

FileSystemAccessManagerImpl::SharedHandleState::SharedHandleState(
    const SharedHandleState& other) = default;
FileSystemAccessManagerImpl::SharedHandleState::~SharedHandleState() = default;

FileSystemAccessManagerImpl::FileSystemAccessManagerImpl(
    scoped_refptr<storage::FileSystemContext> context,
    scoped_refptr<ChromeBlobStorageContext> blob_context,
    FileSystemAccessPermissionContext* permission_context,
    bool off_the_record)
    : context_(std::move(context)),
      blob_context_(std::move(blob_context)),
      permission_context_(permission_context),
      off_the_record_(off_the_record) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  DCHECK(context_);
  DCHECK(blob_context_);
}

FileSystemAccessManagerImpl::~FileSystemAccessManagerImpl() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void FileSystemAccessManagerImpl::BindReceiver(
    const BindingContext& binding_context,
    mojo::PendingReceiver<blink::mojom::FileSystemAccessManager> receiver) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!network::IsOriginPotentiallyTrustworthy(binding_context.origin)) {
    mojo::ReportBadMessage("File System Access access from Unsecure Origin");
    return;
  }

  receivers_.Add(this, std::move(receiver), binding_context);
}

void FileSystemAccessManagerImpl::BindInternalsReceiver(
    mojo::PendingReceiver<storage::mojom::FileSystemAccessContext> receiver) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  internals_receivers_.Add(this, std::move(receiver));
}

void FileSystemAccessManagerImpl::GetSandboxedFileSystem(
    GetSandboxedFileSystemCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto response_callback = base::BindOnce(
      [](base::WeakPtr<FileSystemAccessManagerImpl> manager,
         const BindingContext& binding_context,
         GetSandboxedFileSystemCallback callback,
         scoped_refptr<base::SequencedTaskRunner> task_runner, const GURL& root,
         const std::string& fs_name, base::File::Error result) {
        task_runner->PostTask(
            FROM_HERE,
            base::BindOnce(
                &FileSystemAccessManagerImpl::DidOpenSandboxedFileSystem,
                std::move(manager), binding_context, std::move(callback), root,
                fs_name, result));
      },
      weak_factory_.GetWeakPtr(), receivers_.current_context(),
      std::move(callback), base::SequencedTaskRunnerHandle::Get());

  // TODO(https://crbug.com/1221308): refactor BindingContext to contain
  // StorageKey member; replace StorageKey conversion below with it
  GetIOThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(&FileSystemContext::OpenFileSystem, context(),
                     blink::StorageKey(receivers_.current_context().origin),
                     storage::kFileSystemTypeTemporary,
                     storage::OPEN_FILE_SYSTEM_CREATE_IF_NONEXISTENT,
                     std::move(response_callback)));
}

void FileSystemAccessManagerImpl::ChooseEntries(
    blink::mojom::FilePickerOptionsPtr options,
    blink::mojom::CommonFilePickerOptionsPtr common_options,
    ChooseEntriesCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const BindingContext& context = receivers_.current_context();

  // ChooseEntries API is only available to windows, as we need a frame to
  // anchor the picker to.
  if (context.is_worker()) {
    receivers_.ReportBadMessage("ChooseEntries called from a worker");
    return;
  }

  // Non-compromised renderers shouldn't be able to send an invalid id.
  if (!IsValidId(common_options->starting_directory_id)) {
    receivers_.ReportBadMessage("Invalid starting directory ID in browser");
    return;
  }

  if (permission_context_) {
    // When site setting is block, it's better not to show file chooser.
    // Write permission will be requested for either a save file picker or
    // a directory picker with `request_writable` true.
    if (!permission_context_->CanObtainReadPermission(context.origin) ||
        ((options->is_save_file_picker_options() ||
          (options->is_directory_picker_options() &&
           options->get_directory_picker_options()->request_writable)) &&
         !permission_context_->CanObtainWritePermission(context.origin))) {
      std::move(callback).Run(
          file_system_access_error::FromStatus(
              FileSystemAccessStatus::kPermissionDenied),
          std::vector<blink::mojom::FileSystemAccessEntryPtr>());
      return;
    }
  }

  RenderFrameHost* rfh = RenderFrameHost::FromID(context.frame_id);
  if (!rfh) {
    std::move(callback).Run(
        file_system_access_error::FromStatus(
            FileSystemAccessStatus::kOperationAborted),
        std::vector<blink::mojom::FileSystemAccessEntryPtr>());
    return;
  }

  // Renderer process should already check for user activation before sending
  // IPC, but just to be sure double check here as well. This is not treated
  // as a BadMessage because it is possible for the transient user activation
  // to expire between the renderer side check and this check.
  if (!rfh->HasTransientUserActivation()) {
    std::move(callback).Run(
        file_system_access_error::FromStatus(
            FileSystemAccessStatus::kPermissionDenied,
            "User activation is required to show a file picker."),
        std::vector<blink::mojom::FileSystemAccessEntryPtr>());
    return;
  }

  auto token = std::move(common_options->starting_directory_token);

  auto resolve_default_directory_callback =
      base::BindOnce(&FileSystemAccessManagerImpl::ResolveDefaultDirectory,
                     weak_factory_.GetWeakPtr(), context, std::move(options),
                     std::move(common_options), std::move(callback));

  if (token.is_valid()) {
    ResolveTransferToken(std::move(token),
                         std::move(resolve_default_directory_callback));
    return;
  }

  std::move(resolve_default_directory_callback).Run(/*token=*/nullptr);
}

void FileSystemAccessManagerImpl::ResolveDefaultDirectory(
    const BindingContext& context,
    blink::mojom::FilePickerOptionsPtr options,
    blink::mojom::CommonFilePickerOptionsPtr common_options,
    ChooseEntriesCallback callback,
    FileSystemAccessTransferTokenImpl* resolved_starting_directory_token) {
  PathInfo path_info;

  if (resolved_starting_directory_token)
    HandleTransferTokenAsDefaultDirectory(resolved_starting_directory_token,
                                          path_info);

  if (path_info.path.empty() && permission_context_) {
    if (!common_options->starting_directory_id.empty()) {
      // Prioritize an `id` over a well-known directory.
      path_info = permission_context_->GetLastPickedDirectory(
          context.origin, common_options->starting_directory_id);
    }
    if (path_info.path.empty()) {
      if (common_options->well_known_starting_directory !=
          blink::mojom::WellKnownDirectory::kDefault) {
        // Prioritize an explicitly stated well-known directory over an
        // implicitly remembered LastPicked directory.
        path_info.path = permission_context_->GetWellKnownDirectoryPath(
            common_options->well_known_starting_directory);
      } else { /*well_known_starting_directory ==
                  blink::mojom::WellKnownDirectory::kDefault*/
        // If `id` empty or unset, fall back to the default LastPickedDirectory.
        path_info = permission_context_->GetLastPickedDirectory(context.origin,
                                                                std::string());
      }
    }
  }

  auto fs_url = CreateFileSystemURLFromPath(path_info.type, path_info.path);
  operation_runner()
      .AsyncCall(base::IgnoreResult(
          &storage::FileSystemOperationRunner::DirectoryExists))
      .WithArgs(
          std::move(fs_url),
          base::BindPostTask(
              base::SequencedTaskRunnerHandle::Get(),
              base::BindOnce(
                  &FileSystemAccessManagerImpl::SetDefaultPathAndShowPicker,
                  weak_factory_.GetWeakPtr(), context, std::move(options),
                  std::move(common_options), fs_url.path(),
                  std::move(callback))));
}

void FileSystemAccessManagerImpl::Shutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  permission_context_ = nullptr;
}

void FileSystemAccessManagerImpl::SetDefaultPathAndShowPicker(
    const BindingContext& context,
    blink::mojom::FilePickerOptionsPtr options,
    blink::mojom::CommonFilePickerOptionsPtr common_options,
    base::FilePath default_directory,
    ChooseEntriesCallback callback,
    base::File::Error result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (result != base::File::Error::FILE_OK) {
    // |default_directory| does not exist. Resort to the default.
    if (permission_context_)
      default_directory = permission_context_->GetWellKnownDirectoryPath(
          blink::mojom::WellKnownDirectory::kDefault);
  }

  auto request_directory_write_access =
      options->is_directory_picker_options() &&
      options->get_directory_picker_options()->request_writable;

  auto suggested_name =
      options->is_save_file_picker_options()
          ? options->get_save_file_picker_options()->suggested_name
          : std::string();

  auto suggested_name_path =
      !suggested_name.empty()
          ? net::GenerateFileName(GURL(), std::string(), std::string(),
                                  suggested_name, std::string(), std::string())
          : base::FilePath();

  auto suggested_extension = suggested_name_path.Extension();
  // Our version of `IsShellIntegratedExtension()` is more stringent than
  // the version used in `net::GenerateFileName()`. See
  // `FileSystemChooser::IsShellIntegratedExtension()` for details.
  if (FileSystemChooser::IsShellIntegratedExtension(suggested_extension)) {
    suggested_extension = FILE_PATH_LITERAL("download");
    suggested_name_path =
        suggested_name_path.ReplaceExtension(suggested_extension);
  }

  FileSystemChooser::Options file_system_chooser_options(
      GetSelectFileDialogType(options), GetAndMoveAcceptsTypesInfo(options),
      std::move(default_directory), std::move(suggested_name_path));

  if (auto_file_picker_result_for_test_) {
    DidChooseEntries(context, file_system_chooser_options,
                     common_options->starting_directory_id,
                     request_directory_write_access, std::move(callback),
                     file_system_access_error::Ok(),
                     {*auto_file_picker_result_for_test_});
    return;
  }

  ShowFilePickerOnUIThread(
      context.origin, context.frame_id, file_system_chooser_options,
      base::BindOnce(&FileSystemAccessManagerImpl::DidChooseEntries,
                     weak_factory_.GetWeakPtr(), context,
                     file_system_chooser_options,
                     common_options->starting_directory_id,
                     request_directory_write_access, std::move(callback)));
}

void FileSystemAccessManagerImpl::CreateFileSystemAccessDataTransferToken(
    PathType path_type,
    const base::FilePath& file_path,
    int renderer_id,
    mojo::PendingReceiver<blink::mojom::FileSystemAccessDataTransferToken>
        receiver) {
  auto data_transfer_token_impl =
      std::make_unique<FileSystemAccessDataTransferTokenImpl>(
          this, path_type, file_path, renderer_id, std::move(receiver));
  auto token = data_transfer_token_impl->token();
  data_transfer_tokens_.emplace(token, std::move(data_transfer_token_impl));
}

void FileSystemAccessManagerImpl::GetEntryFromDataTransferToken(
    mojo::PendingRemote<blink::mojom::FileSystemAccessDataTransferToken> token,
    GetEntryFromDataTransferTokenCallback token_resolved_callback) {
  mojo::Remote<blink::mojom::FileSystemAccessDataTransferToken>
      data_transfer_token_remote(std::move(token));

  // Get a failure callback in case this token ends up not being valid (i.e.
  // unrecognized token or wrong renderer process ID).
  mojo::ReportBadMessageCallback failed_token_redemption_callback =
      receivers_.GetBadMessageCallback();

  // Must pass `data_transfer_token_remote` into GetInternalId in order to
  // ensure it stays in scope long enough for the callback to be called.
  auto* raw_data_transfer_token_remote = data_transfer_token_remote.get();
  raw_data_transfer_token_remote->GetInternalId(
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(
              &FileSystemAccessManagerImpl::ResolveDataTransferToken,
              weak_factory_.GetWeakPtr(), std::move(data_transfer_token_remote),
              receivers_.current_context(), std::move(token_resolved_callback),
              std::move(failed_token_redemption_callback)),
          base::UnguessableToken()));
}

void FileSystemAccessManagerImpl::ResolveDataTransferToken(
    mojo::Remote<blink::mojom::FileSystemAccessDataTransferToken>,
    const BindingContext& binding_context,
    GetEntryFromDataTransferTokenCallback token_resolved_callback,
    mojo::ReportBadMessageCallback failed_token_redemption_callback,
    const base::UnguessableToken& token) {
  auto data_transfer_token_impl = data_transfer_tokens_.find(token);

  // Call `token_resolved_callback` with an error if the token isn't registered.
  if (data_transfer_token_impl == data_transfer_tokens_.end()) {
    std::move(failed_token_redemption_callback)
        .Run("Unrecognized drag drop token.");
    return;
  }

  // Call `token_resolved_callback` with an error if the process redeeming the
  // token isn't the same process that the token is registered to.
  if (data_transfer_token_impl->second->renderer_process_id() !=
      binding_context.process_id()) {
    std::move(failed_token_redemption_callback).Run("Invalid renderer ID.");
    return;
  }

  // Look up whether the file path that's associated with the token is a file or
  // directory and call ResolveDataTransferTokenWithFileType with the result.
  auto fs_url = CreateFileSystemURLFromPath(
      data_transfer_token_impl->second->path_type(),
      data_transfer_token_impl->second->file_path());
  operation_runner()
      .AsyncCall(
          base::IgnoreResult(&storage::FileSystemOperationRunner::GetMetadata))
      .WithArgs(fs_url,
                storage::FileSystemOperation::GET_METADATA_FIELD_IS_DIRECTORY,
                base::BindPostTask(
                    base::SequencedTaskRunnerHandle::Get(),
                    base::BindOnce(&HandleTypeFromFileInfo)
                        .Then(base::BindOnce(
                            &FileSystemAccessManagerImpl::
                                ResolveDataTransferTokenWithFileType,
                            weak_factory_.GetWeakPtr(), binding_context,
                            data_transfer_token_impl->second->file_path(),
                            fs_url, std::move(token_resolved_callback)))));
}

void FileSystemAccessManagerImpl::ResolveDataTransferTokenWithFileType(
    const BindingContext& binding_context,
    const base::FilePath& file_path,
    const storage::FileSystemURL& url,
    GetEntryFromDataTransferTokenCallback token_resolved_callback,
    HandleType file_type) {
  SharedHandleState shared_handle_state = GetSharedHandleStateForPath(
      file_path, binding_context.origin, file_type, UserAction::kDragAndDrop);

  blink::mojom::FileSystemAccessEntryPtr entry;
  if (file_type == HandleType::kDirectory) {
    entry = blink::mojom::FileSystemAccessEntry::New(
        blink::mojom::FileSystemAccessHandle::NewDirectory(
            CreateDirectoryHandle(binding_context, url, shared_handle_state)),
        file_path.BaseName().AsUTF8Unsafe());
  } else {
    entry = blink::mojom::FileSystemAccessEntry::New(
        blink::mojom::FileSystemAccessHandle::NewFile(
            CreateFileHandle(binding_context, url, shared_handle_state)),
        file_path.BaseName().AsUTF8Unsafe());
  }

  std::move(token_resolved_callback).Run(std::move(entry));
}

void FileSystemAccessManagerImpl::GetFileHandleFromToken(
    mojo::PendingRemote<blink::mojom::FileSystemAccessTransferToken> token,
    mojo::PendingReceiver<blink::mojom::FileSystemAccessFileHandle>
        file_handle_receiver) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  ResolveTransferToken(
      std::move(token),
      base::BindOnce(
          &FileSystemAccessManagerImpl::DidResolveTransferTokenForFileHandle,
          weak_factory_.GetWeakPtr(), receivers_.current_context(),
          std::move(file_handle_receiver)));
}

void FileSystemAccessManagerImpl::GetDirectoryHandleFromToken(
    mojo::PendingRemote<blink::mojom::FileSystemAccessTransferToken> token,
    mojo::PendingReceiver<blink::mojom::FileSystemAccessDirectoryHandle>
        directory_handle_receiver) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  ResolveTransferToken(
      std::move(token),
      base::BindOnce(&FileSystemAccessManagerImpl::
                         DidResolveTransferTokenForDirectoryHandle,
                     weak_factory_.GetWeakPtr(), receivers_.current_context(),
                     std::move(directory_handle_receiver)));
}

void FileSystemAccessManagerImpl::SerializeHandle(
    mojo::PendingRemote<blink::mojom::FileSystemAccessTransferToken> token,
    SerializeHandleCallback callback) {
  ResolveTransferToken(
      std::move(token),
      base::BindOnce(&FileSystemAccessManagerImpl::DidResolveForSerializeHandle,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

namespace {

std::string SerializePath(const base::FilePath& path) {
  auto path_bytes = base::as_bytes(base::make_span(path.value()));
  return std::string(path_bytes.begin(), path_bytes.end());
}

base::FilePath DeserializePath(const std::string& bytes) {
  base::FilePath::StringType s;
  s.resize(bytes.size() / sizeof(base::FilePath::CharType));
  std::memcpy(&s[0], bytes.data(), s.size() * sizeof(base::FilePath::CharType));
  return base::FilePath(s);
}

}  // namespace

void FileSystemAccessManagerImpl::DidResolveForSerializeHandle(
    SerializeHandleCallback callback,
    FileSystemAccessTransferTokenImpl* resolved_token) {
  if (!resolved_token) {
    std::move(callback).Run({});
    return;
  }

  const storage::FileSystemURL& url = resolved_token->url();

  FileSystemAccessHandleData data;
  data.set_handle_type(resolved_token->type() == HandleType::kFile
                           ? FileSystemAccessHandleData::kFile
                           : FileSystemAccessHandleData::kDirectory);

  if (url.type() == storage::kFileSystemTypeLocal ||
      url.mount_type() == storage::kFileSystemTypeExternal) {
    // A url can have mount_type = external and type = native local at the same
    // time. In that case we want to still treat it as an external path.
    const bool is_external =
        url.mount_type() == storage::kFileSystemTypeExternal;
    content::LocalFileData* file_data =
        is_external ? data.mutable_external() : data.mutable_local();

    base::FilePath url_path = is_external ? url.virtual_path() : url.path();
    base::FilePath root_path = resolved_token->GetWriteGrant()->GetPath();
    if (root_path.empty())
      root_path = url_path;

    file_data->set_root_path(SerializePath(root_path));

    base::FilePath relative_path;
    // We want |relative_path| to be the path of the file or directory
    // relative to |root_path|. FilePath::AppendRelativePath gets us that,
    // but fails if the path we're looking for is equal to the |root_path|.
    // So special case that case (in which case relative path would be empty
    // anyway).
    if (root_path != url_path) {
      bool relative_path_result =
          root_path.AppendRelativePath(url_path, &relative_path);
      DCHECK(relative_path_result);
    }

    file_data->set_relative_path(SerializePath(relative_path));
  } else if (url.type() == storage::kFileSystemTypeTemporary) {
    base::FilePath virtual_path = url.virtual_path();
    data.mutable_sandboxed()->set_virtual_path(SerializePath(virtual_path));

  } else {
    NOTREACHED();
  }

  std::string value;
  bool success = data.SerializeToString(&value);
  DCHECK(success);
  std::vector<uint8_t> result(value.begin(), value.end());
  std::move(callback).Run(result);
}

void FileSystemAccessManagerImpl::DeserializeHandle(
    const url::Origin& origin,
    const std::vector<uint8_t>& bits,
    mojo::PendingReceiver<blink::mojom::FileSystemAccessTransferToken> token) {
  DCHECK(!bits.empty());

  std::string bits_as_string(bits.begin(), bits.end());
  FileSystemAccessHandleData data;
  if (!data.ParseFromString(bits_as_string)) {
    // Drop |token|, and directly return.
    return;
  }

  switch (data.data_case()) {
    case FileSystemAccessHandleData::kSandboxed: {
      base::FilePath virtual_path =
          DeserializePath(data.sandboxed().virtual_path());
      // TODO(https://crbug.com/1221308): replace StorageKey conversion below
      // with the correct StorageKey - most likely from IndexedDB
      storage::FileSystemURL url = context()->CreateCrackedFileSystemURL(
          blink::StorageKey(origin), storage::kFileSystemTypeTemporary,
          virtual_path);

      auto permission_grant =
          base::MakeRefCounted<FixedFileSystemAccessPermissionGrant>(
              PermissionStatus::GRANTED, base::FilePath());
      CreateTransferTokenImpl(
          url, origin, SharedHandleState(permission_grant, permission_grant),
          data.handle_type() == FileSystemAccessHandleData::kDirectory
              ? HandleType::kDirectory
              : HandleType::kFile,
          std::move(token));
      break;
    }
    case FileSystemAccessHandleData::kLocal:
    case FileSystemAccessHandleData::kExternal: {
      const content::LocalFileData& file_data =
          data.data_case() == FileSystemAccessHandleData::kLocal
              ? data.local()
              : data.external();

      base::FilePath root_path = DeserializePath(file_data.root_path());
      base::FilePath relative_path = DeserializePath(file_data.relative_path());
      storage::FileSystemURL root = CreateFileSystemURLFromPath(
          data.data_case() == FileSystemAccessHandleData::kLocal
              ? PathType::kLocal
              : PathType::kExternal,
          root_path);

      storage::FileSystemURL child = context()->CreateCrackedFileSystemURL(
          root.storage_key(), root.mount_type(),
          root.virtual_path().Append(relative_path));

      const bool is_directory =
          data.handle_type() == FileSystemAccessHandleData::kDirectory;

      // Permissions are scoped to |root_path|, rather than the individual
      // handle. So if |relative_path| is not empty, this creates a
      // SharedHandleState for a directory even if the handle represents a
      // file.
      SharedHandleState handle_state = GetSharedHandleStateForPath(
          root_path, origin,
          (is_directory || !relative_path.empty()) ? HandleType::kDirectory
                                                   : HandleType::kFile,
          FileSystemAccessPermissionContext::UserAction::kLoadFromStorage);

      CreateTransferTokenImpl(
          child, origin, handle_state,
          is_directory ? HandleType::kDirectory : HandleType::kFile,
          std::move(token));
      break;
    }
    case FileSystemAccessHandleData::DATA_NOT_SET:
      NOTREACHED();
  }
}

blink::mojom::FileSystemAccessEntryPtr
FileSystemAccessManagerImpl::CreateFileEntryFromPath(
    const BindingContext& binding_context,
    PathType path_type,
    const base::FilePath& file_path,
    UserAction user_action) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  storage::FileSystemURL url =
      CreateFileSystemURLFromPath(path_type, file_path);

  SharedHandleState shared_handle_state = GetSharedHandleStateForPath(
      file_path, binding_context.origin, HandleType::kFile, user_action);

  return blink::mojom::FileSystemAccessEntry::New(
      blink::mojom::FileSystemAccessHandle::NewFile(
          CreateFileHandle(binding_context, url, shared_handle_state)),
      file_path.BaseName().AsUTF8Unsafe());
}

blink::mojom::FileSystemAccessEntryPtr
FileSystemAccessManagerImpl::CreateDirectoryEntryFromPath(
    const BindingContext& binding_context,
    PathType path_type,
    const base::FilePath& file_path,
    UserAction user_action) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  storage::FileSystemURL url =
      CreateFileSystemURLFromPath(path_type, file_path);

  SharedHandleState shared_handle_state = GetSharedHandleStateForPath(
      file_path, binding_context.origin, HandleType::kDirectory, user_action);

  return blink::mojom::FileSystemAccessEntry::New(
      blink::mojom::FileSystemAccessHandle::NewDirectory(
          CreateDirectoryHandle(binding_context, url, shared_handle_state)),
      file_path.BaseName().AsUTF8Unsafe());
}

mojo::PendingRemote<blink::mojom::FileSystemAccessFileHandle>
FileSystemAccessManagerImpl::CreateFileHandle(
    const BindingContext& binding_context,
    const storage::FileSystemURL& url,
    const SharedHandleState& handle_state) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(url.is_valid());

  mojo::PendingRemote<blink::mojom::FileSystemAccessFileHandle> result;
  file_receivers_.Add(std::make_unique<FileSystemAccessFileHandleImpl>(
                          this, binding_context, url, handle_state),
                      result.InitWithNewPipeAndPassReceiver());
  return result;
}

mojo::PendingRemote<blink::mojom::FileSystemAccessDirectoryHandle>
FileSystemAccessManagerImpl::CreateDirectoryHandle(
    const BindingContext& binding_context,
    const storage::FileSystemURL& url,
    const SharedHandleState& handle_state) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(url.is_valid());

  mojo::PendingRemote<blink::mojom::FileSystemAccessDirectoryHandle> result;
  directory_receivers_.Add(
      std::make_unique<FileSystemAccessDirectoryHandleImpl>(
          this, binding_context, url, handle_state),
      result.InitWithNewPipeAndPassReceiver());
  return result;
}

FileSystemAccessManagerImpl::WriteLockManager::WriteLockManager() = default;

FileSystemAccessManagerImpl::WriteLockManager::~WriteLockManager() = default;

bool FileSystemAccessManagerImpl::WriteLockManager::AddAccessHandle(
    const storage::FileSystemURL& url,
    std::unique_ptr<FileSystemAccessAccessHandleHostImpl> access_handle) {
  DCHECK(url.type() == storage::kFileSystemTypeTemporary);

  // TODO(fivedots): Verify that there are no active writers for `url`, once we
  // implement Add/RemoveWriter.
  auto insert_result =
      access_handle_receivers_.emplace(url, std::move(access_handle));
  bool insert_success = insert_result.second;
  return insert_success;
}

bool FileSystemAccessManagerImpl::WriteLockManager::AddWriter(
    const storage::FileSystemURL& url,
    std::unique_ptr<FileSystemAccessFileWriterImpl> writer) {
  DCHECK(url.type() == storage::kFileSystemTypeTemporary);

  // TODO(fivedots): implement this method and migrate ownership of writers.
  NOTIMPLEMENTED();
  return false;
}

void FileSystemAccessManagerImpl::WriteLockManager::RemoveAccessHandle(
    const storage::FileSystemURL& url) {
  DCHECK(url.type() == storage::kFileSystemTypeTemporary);

  size_t count_removed = access_handle_receivers_.erase(url);
  DCHECK_EQ(1u, count_removed);
}

void FileSystemAccessManagerImpl::WriteLockManager::RemoveWriter(
    const storage::FileSystemURL& url) {
  DCHECK(url.type() == storage::kFileSystemTypeTemporary);

  // TODO(fivedots): implement this method and migrate ownership of writers.
  NOTIMPLEMENTED();
}

mojo::PendingRemote<blink::mojom::FileSystemAccessFileWriter>
FileSystemAccessManagerImpl::CreateFileWriter(
    const BindingContext& binding_context,
    const storage::FileSystemURL& url,
    const storage::FileSystemURL& swap_url,
    const SharedHandleState& handle_state,
    bool auto_close) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  mojo::PendingRemote<blink::mojom::FileSystemAccessFileWriter> result;

  RenderFrameHost* rfh = RenderFrameHost::FromID(binding_context.frame_id);
  bool has_transient_user_activation = rfh && rfh->HasTransientUserActivation();

  CreateFileWriter(
      binding_context, url, swap_url, handle_state,
      result.InitWithNewPipeAndPassReceiver(), has_transient_user_activation,
      auto_close,
      GetContentClient()->browser()->GetQuarantineConnectionCallback());
  return result;
}

base::WeakPtr<FileSystemAccessFileWriterImpl>
FileSystemAccessManagerImpl::CreateFileWriter(
    const BindingContext& binding_context,
    const storage::FileSystemURL& url,
    const storage::FileSystemURL& swap_url,
    const SharedHandleState& handle_state,
    mojo::PendingReceiver<blink::mojom::FileSystemAccessFileWriter> receiver,
    bool has_transient_user_activation,
    bool auto_close,
    download::QuarantineConnectionCallback quarantine_connection_callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto writer = std::make_unique<FileSystemAccessFileWriterImpl>(
      this, PassKey(), binding_context, url, swap_url, handle_state,
      std::move(receiver), has_transient_user_activation, auto_close,
      quarantine_connection_callback);

  base::WeakPtr<FileSystemAccessFileWriterImpl> writer_weak =
      writer->weak_ptr();
  writer_receivers_.insert(std::move(writer));

  return writer_weak;
}

mojo::PendingRemote<blink::mojom::FileSystemAccessAccessHandleHost>
FileSystemAccessManagerImpl::CreateAccessHandleHost(
    const storage::FileSystemURL& url,
    mojo::PendingReceiver<blink::mojom::FileSystemAccessFileDelegateHost>
        file_delegate_receiver) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  mojo::PendingRemote<blink::mojom::FileSystemAccessAccessHandleHost> result;
  auto receiver = result.InitWithNewPipeAndPassReceiver();
  auto access_handle_host =
      std::make_unique<FileSystemAccessAccessHandleHostImpl>(
          this, url, PassKey(), std::move(receiver),
          std::move(file_delegate_receiver));
  auto success =
      write_lock_manager_.AddAccessHandle(url, std::move(access_handle_host));
  if (!success) {
    return mojo::NullRemote();
  }

  return result;
}

void FileSystemAccessManagerImpl::CreateTransferToken(
    const FileSystemAccessFileHandleImpl& file,
    mojo::PendingReceiver<blink::mojom::FileSystemAccessTransferToken>
        receiver) {
  return CreateTransferTokenImpl(file.url(), file.context().origin,
                                 file.handle_state(), HandleType::kFile,
                                 std::move(receiver));
}

void FileSystemAccessManagerImpl::CreateTransferToken(
    const FileSystemAccessDirectoryHandleImpl& directory,
    mojo::PendingReceiver<blink::mojom::FileSystemAccessTransferToken>
        receiver) {
  return CreateTransferTokenImpl(directory.url(), directory.context().origin,
                                 directory.handle_state(),
                                 HandleType::kDirectory, std::move(receiver));
}

void FileSystemAccessManagerImpl::ResolveTransferToken(
    mojo::PendingRemote<blink::mojom::FileSystemAccessTransferToken> token,
    ResolvedTokenCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  mojo::Remote<blink::mojom::FileSystemAccessTransferToken> token_remote(
      std::move(token));
  auto* raw_token = token_remote.get();
  raw_token->GetInternalID(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
      base::BindOnce(&FileSystemAccessManagerImpl::DoResolveTransferToken,
                     weak_factory_.GetWeakPtr(), std::move(token_remote),
                     std::move(callback)),
      base::UnguessableToken()));
}

void FileSystemAccessManagerImpl::DidResolveTransferTokenForFileHandle(
    const BindingContext& binding_context,
    mojo::PendingReceiver<blink::mojom::FileSystemAccessFileHandle>
        file_handle_receiver,
    FileSystemAccessTransferTokenImpl* resolved_token) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!IsValidTransferToken(resolved_token, binding_context.origin,
                            HandleType::kFile)) {
    // Fail silently. In practice, the FileSystemAccessManager should not
    // receive any invalid tokens. Before redeeming a token, the render process
    // performs an origin check to ensure the token is valid. Invalid tokens
    // indicate a code bug or a compromised render process.
    //
    // After receiving an invalid token, the FileSystemAccessManager
    // cannot determine which render process is compromised. Is it the post
    // message sender or receiver? Because of this, the FileSystemAccessManager
    // closes the FileHandle pipe and ignores the error.
    return;
  }

  file_receivers_.Add(resolved_token->CreateFileHandle(binding_context),
                      std::move(file_handle_receiver));
}

void FileSystemAccessManagerImpl::DidResolveTransferTokenForDirectoryHandle(
    const BindingContext& binding_context,
    mojo::PendingReceiver<blink::mojom::FileSystemAccessDirectoryHandle>
        directory_handle_receiver,
    FileSystemAccessTransferTokenImpl* resolved_token) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!IsValidTransferToken(resolved_token, binding_context.origin,
                            HandleType::kDirectory)) {
    // Fail silently. See comment above in
    // DidResolveTransferTokenForFileHandle() for details.
    return;
  }

  directory_receivers_.Add(
      resolved_token->CreateDirectoryHandle(binding_context),
      std::move(directory_handle_receiver));
}

const base::SequenceBound<storage::FileSystemOperationRunner>&
FileSystemAccessManagerImpl::operation_runner() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!operation_runner_) {
    operation_runner_ =
        context()->CreateSequenceBoundFileSystemOperationRunner();
  }
  return operation_runner_;
}

void FileSystemAccessManagerImpl::DidOpenSandboxedFileSystem(
    const BindingContext& binding_context,
    GetSandboxedFileSystemCallback callback,
    const GURL& root,
    const std::string& filesystem_name,
    base::File::Error result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (result != base::File::FILE_OK) {
    std::move(callback).Run(file_system_access_error::FromFileError(result),
                            mojo::NullRemote());
    return;
  }

  auto permission_grant =
      base::MakeRefCounted<FixedFileSystemAccessPermissionGrant>(
          PermissionStatus::GRANTED, base::FilePath());

  // TODO(https://crbug.com/1221308): determine whether StorageKey should be
  // replaced with a more meaningful value
  std::move(callback).Run(
      file_system_access_error::Ok(),
      CreateDirectoryHandle(
          binding_context,
          context()->CrackURL(root,
                              blink::StorageKey(url::Origin::Create(root))),
          SharedHandleState(permission_grant, permission_grant)));
}

void FileSystemAccessManagerImpl::DidChooseEntries(
    const BindingContext& binding_context,
    const FileSystemChooser::Options& options,
    const std::string& starting_directory_id,
    const bool request_directory_write_access,
    ChooseEntriesCallback callback,
    blink::mojom::FileSystemAccessErrorPtr result,
    std::vector<FileSystemChooser::ResultEntry> entries) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (result->status != FileSystemAccessStatus::kOk || entries.empty()) {
    std::move(callback).Run(
        std::move(result),
        std::vector<blink::mojom::FileSystemAccessEntryPtr>());
    return;
  }

  if (!permission_context_) {
    DidVerifySensitiveDirectoryAccess(
        binding_context, options, starting_directory_id,
        request_directory_write_access, std::move(callback), std::move(entries),
        SensitiveDirectoryResult::kAllowed);
    return;
  }

  // It is enough to only verify access to the first path, as multiple
  // file selection is only supported if all files are in the same
  // directory.
  FileSystemChooser::ResultEntry first_entry = entries.front();
  const bool is_directory =
      options.type() == ui::SelectFileDialog::SELECT_FOLDER;
  permission_context_->ConfirmSensitiveDirectoryAccess(
      binding_context.origin, first_entry.type, first_entry.path,
      is_directory ? HandleType::kDirectory : HandleType::kFile,
      binding_context.frame_id,
      base::BindOnce(
          &FileSystemAccessManagerImpl::DidVerifySensitiveDirectoryAccess,
          weak_factory_.GetWeakPtr(), binding_context, options,
          starting_directory_id, request_directory_write_access,
          std::move(callback), std::move(entries)));
}

void FileSystemAccessManagerImpl::DidVerifySensitiveDirectoryAccess(
    const BindingContext& binding_context,
    const FileSystemChooser::Options& options,
    const std::string& starting_directory_id,
    const bool request_directory_write_access,
    ChooseEntriesCallback callback,
    std::vector<FileSystemChooser::ResultEntry> entries,
    SensitiveDirectoryResult result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  base::UmaHistogramEnumeration(
      "NativeFileSystemAPI.SensitiveDirectoryAccessResult", result);

  if (result == SensitiveDirectoryResult::kAbort) {
    std::move(callback).Run(
        file_system_access_error::FromStatus(
            FileSystemAccessStatus::kOperationAborted),
        std::vector<blink::mojom::FileSystemAccessEntryPtr>());
    return;
  }
  if (result == SensitiveDirectoryResult::kTryAgain) {
    ShowFilePickerOnUIThread(
        binding_context.origin, binding_context.frame_id, options,
        base::BindOnce(&FileSystemAccessManagerImpl::DidChooseEntries,
                       weak_factory_.GetWeakPtr(), binding_context, options,
                       starting_directory_id, request_directory_write_access,
                       std::move(callback)));
    return;
  }

  if (permission_context_ && !entries.empty()) {
    auto picked_directory =
        options.type() == ui::SelectFileDialog::SELECT_FOLDER
            ? entries.front().path
            : entries.front().path.DirName();
    permission_context_->SetLastPickedDirectory(
        binding_context.origin, starting_directory_id, picked_directory,
        entries.front().type);
  }

  if (options.type() == ui::SelectFileDialog::SELECT_FOLDER) {
    DCHECK_EQ(entries.size(), 1u);
    SharedHandleState shared_handle_state = GetSharedHandleStateForPath(
        entries.front().path, binding_context.origin, HandleType::kDirectory,
        FileSystemAccessPermissionContext::UserAction::kOpen);
    // Ask for both read and write permission at the same time. The permission
    // context should coalesce these into one prompt.
    if (request_directory_write_access) {
      shared_handle_state.write_grant->RequestPermission(
          binding_context.frame_id,
          FileSystemAccessPermissionGrant::UserActivationState::kNotRequired,
          base::DoNothing());
    }
    shared_handle_state.read_grant->RequestPermission(
        binding_context.frame_id,
        FileSystemAccessPermissionGrant::UserActivationState::kNotRequired,
        base::BindOnce(&FileSystemAccessManagerImpl::DidChooseDirectory, this,
                       binding_context, entries.front(), std::move(callback),
                       shared_handle_state));
    return;
  }

  if (options.type() == ui::SelectFileDialog::SELECT_SAVEAS_FILE) {
    DCHECK_EQ(entries.size(), 1u);
    // Create file if it doesn't yet exist, and truncate file if it does exist.
    auto fs_url =
        CreateFileSystemURLFromPath(entries.front().type, entries.front().path);

    operation_runner().PostTaskWithThisObject(
        FROM_HERE,
        base::BindOnce(
            &CreateAndTruncateFile, fs_url,
            base::BindOnce(
                &FileSystemAccessManagerImpl::DidCreateAndTruncateSaveFile,
                this, binding_context, entries.front(), fs_url,
                std::move(callback)),
            base::SequencedTaskRunnerHandle::Get()));
    return;
  }

  std::vector<blink::mojom::FileSystemAccessEntryPtr> result_entries;
  result_entries.reserve(entries.size());
  for (const auto& entry : entries) {
    result_entries.push_back(CreateFileEntryFromPath(
        binding_context, entry.type, entry.path, UserAction::kOpen));
  }
  std::move(callback).Run(file_system_access_error::Ok(),
                          std::move(result_entries));
}

void FileSystemAccessManagerImpl::DidCreateAndTruncateSaveFile(
    const BindingContext& binding_context,
    const FileSystemChooser::ResultEntry& entry,
    const storage::FileSystemURL& url,
    ChooseEntriesCallback callback,
    bool success) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::vector<blink::mojom::FileSystemAccessEntryPtr> result_entries;
  if (!success) {
    // TODO(https://crbug.com/1124871): Failure to create or truncate the file
    // should probably not just result in a generic error, but instead inform
    // the user of the problem?
    std::move(callback).Run(
        file_system_access_error::FromStatus(
            blink::mojom::FileSystemAccessStatus::kOperationFailed,
            "Failed to create or truncate file"),
        std::move(result_entries));
    return;
  }

  SharedHandleState shared_handle_state = GetSharedHandleStateForPath(
      entry.path, binding_context.origin, HandleType::kFile, UserAction::kSave);

  result_entries.push_back(blink::mojom::FileSystemAccessEntry::New(
      blink::mojom::FileSystemAccessHandle::NewFile(
          CreateFileHandle(binding_context, url, shared_handle_state)),
      entry.path.BaseName().AsUTF8Unsafe()));

  std::move(callback).Run(file_system_access_error::Ok(),
                          std::move(result_entries));
}

void FileSystemAccessManagerImpl::DidChooseDirectory(
    const BindingContext& binding_context,
    const FileSystemChooser::ResultEntry& entry,
    ChooseEntriesCallback callback,
    const SharedHandleState& shared_handle_state,
    FileSystemAccessPermissionGrant::PermissionRequestOutcome outcome) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  base::UmaHistogramEnumeration(
      "NativeFileSystemAPI.ConfirmReadDirectoryResult",
      shared_handle_state.read_grant->GetStatus());

  std::vector<blink::mojom::FileSystemAccessEntryPtr> result_entries;
  if (shared_handle_state.read_grant->GetStatus() !=
      PermissionStatus::GRANTED) {
    std::move(callback).Run(file_system_access_error::FromStatus(
                                FileSystemAccessStatus::kOperationAborted),
                            std::move(result_entries));
    return;
  }

  storage::FileSystemURL url =
      CreateFileSystemURLFromPath(entry.type, entry.path);

  result_entries.push_back(blink::mojom::FileSystemAccessEntry::New(
      blink::mojom::FileSystemAccessHandle::NewDirectory(CreateDirectoryHandle(
          binding_context, url,
          SharedHandleState(shared_handle_state.read_grant,
                            shared_handle_state.write_grant))),
      entry.path.BaseName().AsUTF8Unsafe()));
  std::move(callback).Run(file_system_access_error::Ok(),
                          std::move(result_entries));
}

void FileSystemAccessManagerImpl::CreateTransferTokenImpl(
    const storage::FileSystemURL& url,
    const url::Origin& origin,
    const SharedHandleState& handle_state,
    HandleType handle_type,
    mojo::PendingReceiver<blink::mojom::FileSystemAccessTransferToken>
        receiver) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto token_impl = std::make_unique<FileSystemAccessTransferTokenImpl>(
      url, origin, handle_state, handle_type, this, std::move(receiver));
  auto token = token_impl->token();
  transfer_tokens_.emplace(token, std::move(token_impl));
}

void FileSystemAccessManagerImpl::RemoveFileWriter(
    FileSystemAccessFileWriterImpl* writer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  size_t count_removed = writer_receivers_.erase(writer);
  DCHECK_EQ(1u, count_removed);
}

void FileSystemAccessManagerImpl::RemoveAccessHandleHost(
    const storage::FileSystemURL& url) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  write_lock_manager_.RemoveAccessHandle(url);
}

void FileSystemAccessManagerImpl::RemoveToken(
    const base::UnguessableToken& token) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  size_t count_removed = transfer_tokens_.erase(token);
  DCHECK_EQ(1u, count_removed);
}

void FileSystemAccessManagerImpl::RemoveDataTransferToken(
    const base::UnguessableToken& token) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  size_t count_removed = data_transfer_tokens_.erase(token);
  DCHECK_EQ(1u, count_removed);
}

void FileSystemAccessManagerImpl::DoResolveTransferToken(
    mojo::Remote<blink::mojom::FileSystemAccessTransferToken>,
    ResolvedTokenCallback callback,
    const base::UnguessableToken& token) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto it = transfer_tokens_.find(token);
  if (it == transfer_tokens_.end()) {
    std::move(callback).Run(nullptr);
  } else {
    std::move(callback).Run(it->second.get());
  }
}

storage::FileSystemURL FileSystemAccessManagerImpl::CreateFileSystemURLFromPath(
    PathType path_type,
    const base::FilePath& path) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return context()->CreateCrackedFileSystemURL(
      blink::StorageKey(),
      path_type == PathType::kLocal ? storage::kFileSystemTypeLocal
                                    : storage::kFileSystemTypeExternal,
      path);
}

FileSystemAccessManagerImpl::SharedHandleState
FileSystemAccessManagerImpl::GetSharedHandleStateForPath(
    const base::FilePath& path,
    const url::Origin& origin,
    HandleType handle_type,
    FileSystemAccessPermissionContext::UserAction user_action) {
  scoped_refptr<FileSystemAccessPermissionGrant> read_grant, write_grant;
  if (permission_context_) {
    read_grant = permission_context_->GetReadPermissionGrant(
        origin, path, handle_type, user_action);
    write_grant = permission_context_->GetWritePermissionGrant(
        origin, path, handle_type, user_action);
  } else {
    // Auto-deny all write grants if no permisson context is available, unless
    // Experimental Web Platform features are enabled.
    // TODO(mek): Remove experimental web platform check when permission UI is
    // implemented.
    write_grant = base::MakeRefCounted<FixedFileSystemAccessPermissionGrant>(
        base::CommandLine::ForCurrentProcess()->HasSwitch(
            switches::kEnableExperimentalWebPlatformFeatures)
            ? PermissionStatus::GRANTED
            : PermissionStatus::DENIED,
        path);
    if (user_action ==
        FileSystemAccessPermissionContext::UserAction::kLoadFromStorage) {
      read_grant = write_grant;
    } else {
      // Grant read permission even without a permission_context_, as the picker
      // itself is enough UI to assume user intent.
      read_grant = base::MakeRefCounted<FixedFileSystemAccessPermissionGrant>(
          PermissionStatus::GRANTED, path);
    }
  }
  return SharedHandleState(std::move(read_grant), std::move(write_grant));
}

}  // namespace content
