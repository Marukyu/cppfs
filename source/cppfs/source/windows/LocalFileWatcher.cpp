
#include <cppfs/windows/LocalFileWatcher.h>

#include <algorithm>

#include <cppfs/FilePath.h>
#include <cppfs/windows/LocalFileSystem.h>
#include <cppfs/windows/FileNameConversions.h>


namespace
{
    /**
    *  @brief
    *    Helper class to lock/unlock a critical section automatically
    */
    struct ScopedCriticalSection
    {
        // NB! Using a std::mutex resulted in spurious deadlocks using VS2017, whilst
        // using CRITICAL_SECTION works fine.
        ::LPCRITICAL_SECTION m_pcs;

        ScopedCriticalSection(::LPCRITICAL_SECTION pcs)
        : m_pcs(pcs)
        {
            ::EnterCriticalSection(m_pcs);
        }

        ~ScopedCriticalSection()
        {
            ::LeaveCriticalSection(m_pcs);
        }
    };
}


namespace cppfs
{


LocalFileWatcher::LocalFileWatcher(FileWatcher * fileWatcher, std::shared_ptr<LocalFileSystem> fs)
: AbstractFileWatcherBackend(fileWatcher)
, m_fs(fs)
, m_waitStopEvent(::CreateEventW(NULL, TRUE, FALSE, NULL))
{
    // Create critical section
    ::InitializeCriticalSection(&m_mutexWatchers);
}

LocalFileWatcher::~LocalFileWatcher()
{
    // Release critical section
    ::DeleteCriticalSection(&m_mutexWatchers);
    m_watchers.clear();

    // Release stop-event
    ::CloseHandle(m_waitStopEvent);
}

AbstractFileSystem * LocalFileWatcher::fs() const
{
    return static_cast<AbstractFileSystem *>(m_fs.get());
}

void LocalFileWatcher::add(FileHandle & dir, unsigned int events, RecursiveMode recursive)
{
    // Open directory
    ::HANDLE dirHandle = ::CreateFileW(
        convert::utf8ToWideString(dir.path()).c_str(),          // Pointer to the directory name
        FILE_LIST_DIRECTORY,                                    // Access (read/write) mode
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, // File share mode
        NULL,                                                   // Security descriptor
        OPEN_EXISTING,                                          // How to create
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,      // File attributes
        NULL                                                    // File with attributes to copy
    );

    // Check if directory could be opened
    if (dirHandle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("failed to create directory listener");
    }

    // Create new watcher
    std::unique_ptr<Watcher> watcher(new Watcher);
    watcher->dir        = dir;
    watcher->events     = events;
    watcher->recursive  = recursive;
    watcher->initialized = false;
    watcher->dirHandle  = std::shared_ptr<void>(dirHandle, ::CloseHandle);
    watcher->event      = std::shared_ptr<void>(::CreateEventW(NULL, TRUE, FALSE, NULL), ::CloseHandle);
    watcher->overlapped.hEvent = watcher->event.get();
    watcher->buffer.resize(1024 * sizeof(FILE_NOTIFY_INFORMATION));

    // Check if event could be created
    if (!watcher->event) {
        throw std::runtime_error("failed creating wait event");
    }

    // Signal watch function that something has changed
    ::SetEvent(m_waitStopEvent);

    // Add watcher to list
    ScopedCriticalSection lock(&m_mutexWatchers);
    m_watchers.push_back(std::move(*watcher.release()));
}

void LocalFileWatcher::watch(int timeout)
{
    ScopedCriticalSection lock(&m_mutexWatchers);

    // Initialize list of handles to wait for
    std::vector<::HANDLE> waitHandles;
    waitHandles.push_back(m_waitStopEvent); // This is now WAIT_OBJECT_0

    // Process all watcher
    for (auto & watcher : m_watchers) {
        // Add event handle
        waitHandles.push_back(watcher.event.get());

        // Check if data is available
        DWORD size = 0;
        if (!::GetOverlappedResult(watcher.dirHandle.get(), &watcher.overlapped, &size, FALSE)) {
            auto error = GetLastError();
            if (error == ERROR_IO_INCOMPLETE) {
                continue;
            }
        }

        // Initialize watcher if necessary
        if (!watcher.initialized)
        {
            createWatcher(watcher);
            watcher.initialized = true;
        }
    }

    // Wait for events to happen
    auto waitResult = ::WaitForMultipleObjects(
        waitHandles.size(),
        waitHandles.data(),
        FALSE,
        timeout >= 0 ? timeout : INFINITE
    );

    // Check for timeout
    if (waitResult == WAIT_TIMEOUT) {
        return;
    }

    // Check for waitStopEvent
    if (waitResult == WAIT_OBJECT_0) {
        // Watchers have been modified, leave watch function
        ::ResetEvent(m_waitStopEvent);
        return;
    }

    // Get index of watcher that has fired
    auto index = waitResult - (WAIT_OBJECT_0 + 1);
    if (index < 0 || index >= (int)m_watchers.size()) {
        // Invalid watcher index
        throw std::runtime_error("Wait returned result: " + std::to_string(waitResult));
    }

    // Get watcher
    Watcher & watcher = m_watchers[index];
    ::ResetEvent(watcher.event.get());

    // Read events
    DWORD size = 0;
    if (::GetOverlappedResult(watcher.dirHandle.get(), &watcher.overlapped, &size, FALSE) && size > 0) {

        // Process events
        char * entry = reinterpret_cast<char *>(watcher.buffer.data());
        while (entry) {
            // Get file event notification
            FILE_NOTIFY_INFORMATION * fileEvent = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(entry);

            // Convert filename to UTF-8
            std::string fileName = convert::wideToUtf8String(std::wstring(fileEvent->FileName,
                fileEvent->FileNameLength / sizeof(wchar_t)));

            // Check if conversion was successful
            if (!fileName.empty()) {
                // Get filename in unified format
                std::string fname = FilePath(fileName).path();

                // Determine event type
                FileEvent eventType = (FileEvent)0;
                switch (fileEvent->Action) {
                    case FILE_ACTION_ADDED:
                        eventType = FileCreated;
                        break;

                    case FILE_ACTION_REMOVED:
                        eventType = FileRemoved;
                        break;

                    case FILE_ACTION_MODIFIED:
                        eventType = FileModified;
                        break;

                    case FILE_ACTION_RENAMED_OLD_NAME:
                        eventType = FileMovedFrom;
                        break;

                    case FILE_ACTION_RENAMED_NEW_NAME:
                        eventType = FileMovedTo;
                        break;

                    default:
                        break;
                }

                // Check if event is watched for
                if (watcher.events & eventType) {
                    // Get file handle
                    FileHandle fh = watcher.dir.open(fname);

                    // Invoke callback function
                    onFileEvent(fh, eventType);
                }
            }

            // Get next event
            if (fileEvent->NextEntryOffset != 0) {
                entry += fileEvent->NextEntryOffset;
            } else {
                entry = nullptr;
            }
        }

        // Re-initialize watcher
        createWatcher(watcher);
    }
}

void LocalFileWatcher::createWatcher(Watcher & watcher)
{
    // Get events to watch for
    DWORD flags = 0;
    if (watcher.events & (FileCreated | FileRemoved | FileMovedFrom | FileMovedTo))
        flags |= FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
    if (watcher.events & FileModified)    flags |= FILE_NOTIFY_CHANGE_LAST_WRITE;
    if (watcher.events & FileAttrChanged) flags |= FILE_NOTIFY_CHANGE_ATTRIBUTES;

    // Create watcher
    if (!::ReadDirectoryChangesW(
        watcher.dirHandle.get(),
        watcher.buffer.data(),
        watcher.buffer.size(),
        watcher.recursive == Recursive,
        flags,
        NULL,
        &watcher.overlapped,
        NULL))
    {
        // Error creating the watcher
        auto error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            throw std::runtime_error("Error calling ReadDirectoryChangesW: " + std::to_string(error));
        }
    }
}

} // namespace cppfs
