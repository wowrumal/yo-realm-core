// Memory Mapping includes
#ifdef _MSC_VER
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <conio.h>
#include <stdio.h>
#else
#include <unistd.h> // close()
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/file.h>
#endif

#include <iostream>

#include <tightdb/alloc_slab.hpp>
#include <tightdb/array.hpp>

#ifdef TIGHTDB_DEBUG
#include <cstdio>
#endif

using namespace std;
using namespace tightdb;

namespace {

// Support function
// todo, fixme: use header function in array instead!
size_t GetCapacityFromHeader(void* p)
{
    // parse the capacity part of 8byte header
    const uint8_t* const header = (uint8_t*)p;
    return (header[4] << 16) + (header[5] << 8) + header[6];
}

size_t GetSizeFromHeader(void* p)
{
    // parse width and count from 8byte header
    const uint8_t* const header = (uint8_t*)p;
    const size_t width = (1 << (header[0] & 0x07)) >> 1;
    const size_t count = (header[1] << 16) + (header[2] << 8) + header[3];
    const size_t wt    = (header[0] & 0x18) >> 3; // Array::WidthType

    // Calculate bytes used by array
    size_t bytes = 0;
    if (wt == 0) { // TDB_BITS
        const size_t bits = (count * width);
        bytes = bits / 8;
        if (bits & 0x7) ++bytes; // include partial bytes
    }
    else if (wt == 1) { // TDB_MULTIPLY
        bytes = count * width;
    }
    else if (wt == 2) { // TDB_IGNORE
        bytes = count;
    }
    else TIGHTDB_ASSERT(false);

    // Arrays are always padded to 64 bit alignment
    const size_t rest = (~bytes & 0x7)+1;
    if (rest < 8) bytes += rest; // 64bit blocks

    // include header in total
    bytes += 8;

    return bytes;
}

} // anonymous namespace


namespace tightdb {

Allocator& GetDefaultAllocator()
{
    static Allocator DefaultAllocator;
    return DefaultAllocator;
}

SlabAlloc::SlabAlloc(): m_shared(NULL), m_owned(false), m_baseline(8)
{
#ifdef TIGHTDB_DEBUG
    m_debugOut = false;
#endif
}

SlabAlloc::~SlabAlloc()
{
#ifdef TIGHTDB_DEBUG
    if (!IsAllFree()) {
        m_slabs.print();
        m_freeSpace.print();
        TIGHTDB_ASSERT(false);  // FIXME: Should this assert be here?
    }
#endif // TIGHTDB_DEBUG

    // Release all allocated memory
    for (size_t i = 0; i < m_slabs.size(); ++i) {
        void* p = (void*)(intptr_t)m_slabs[i].pointer;
        free(p);
    }

    // Release any shared memory
    if (m_shared) {
        if (m_owned) {
            free(m_shared);
        }
        else {
#ifdef _MSC_VER
            UnmapViewOfFile(m_shared);
            CloseHandle(m_fd);
            CloseHandle(m_mapfile);
#else
            munmap(m_shared, m_baseline);
            close(m_fd);
#endif
        }
    }
}

MemRef SlabAlloc::Alloc(size_t size)
{
    TIGHTDB_ASSERT((size & 0x7) == 0); // only allow sizes that are multibles of 8

    // Do we have a free space we can reuse?
    const size_t count = m_freeSpace.size();
    for (size_t i = 0; i < count; ++i) {
        FreeSpace::Cursor r = m_freeSpace[i];
        if (r.size >= (int)size) {
            const size_t location = (size_t)r.ref;
            const size_t rest = (size_t)r.size - size;

            // Update free list
            if (rest == 0) m_freeSpace.remove(i);
            else {
                r.size = rest;
                r.ref += (unsigned int)size;
            }

#ifdef TIGHTDB_DEBUG
            if (m_debugOut) {
                printf("Alloc ref: %lu size: %lu\n", location, size);
            }
#endif // TIGHTDB_DEBUG

            void* const pointer = Translate(location);
            return MemRef(pointer, location);
        }
    }

    // Else, allocate new slab
    const size_t multible = 256 * ((size / 256) + 1); // FIXME: Not an english word. Also, is this the intended rounding behavior?
    const size_t slabsBack = m_slabs.is_empty() ? m_baseline : size_t(m_slabs.back().offset);
    const size_t doubleLast = m_slabs.is_empty() ? 0 :
        (slabsBack - ((m_slabs.size() == 1) ? size_t(0) : size_t(m_slabs.back(-2).offset))) * 2;
    const size_t newsize = multible > doubleLast ? multible : doubleLast;

    // Allocate memory
    void* const slab = newsize ? malloc(newsize): NULL;
    if (!slab) return MemRef(NULL, 0);

    // Add to slab table
    Slabs::Cursor s = m_slabs.add(); // FIXME: Use the immediate form add()
    s.offset = slabsBack + newsize;
    s.pointer = (intptr_t)slab;

    // Update free list
    const size_t rest = newsize - size;
    FreeSpace::Cursor f = m_freeSpace.add(); // FIXME: Use the immediate form add()
    f.ref = slabsBack + size;
    f.size = rest;

#ifdef TIGHTDB_DEBUG
    if (m_debugOut) {
        printf("Alloc ref: %lu size: %lu\n", slabsBack, size);
    }
#endif // TIGHTDB_DEBUG

    return MemRef(slab, slabsBack);
}

void SlabAlloc::Free(size_t ref, void* p)
{
    // Free space in read only segment is tracked separately
    const bool isReadOnly = IsReadOnly(ref);
    FreeSpace& freeSpace = isReadOnly ? m_freeReadOnly : m_freeSpace;

    // Get size from segment
    const size_t size = isReadOnly ? GetSizeFromHeader(p) : GetCapacityFromHeader(p);
    const size_t refEnd = ref + size;
    bool isMerged = false;

#ifdef TIGHTDB_DEBUG
    if (m_debugOut) {
        printf("Free ref: %lu size: %lu\n", ref, size);
    }
#endif // TIGHTDB_DEBUG

    // Check if we can merge with start of free block
    const size_t n = freeSpace.column().ref.find_first(refEnd);
    if (n != (size_t)-1) {
        // No consolidation over slab borders
        if (m_slabs.column().offset.find_first(refEnd) == (size_t)-1) {
            freeSpace[n].ref = ref;
            freeSpace[n].size += size;
            isMerged = true;
        }
    }

    // Check if we can merge with end of free block
    if (m_slabs.column().offset.find_first(ref) == (size_t)-1) { // avoid slab borders
        const size_t count = freeSpace.size();
        for (size_t i = 0; i < count; ++i) {
            FreeSpace::Cursor c = freeSpace[i];

        //  printf("%d %d", c.ref, c.size);

            const size_t end = to_ref(c.ref + c.size);
            if (ref == end) {
                if (isMerged) {
                    c.size += freeSpace[n].size;
                    freeSpace.remove(n);
                }
                else c.size += size;

                return;
            }
        }
    }

    // Else just add to freelist
    if (!isMerged) freeSpace.add(ref, size);
}

MemRef SlabAlloc::ReAlloc(size_t ref, void* p, size_t size)
{
    TIGHTDB_ASSERT((size & 0x7) == 0); // only allow sizes that are multibles of 8

    //TODO: Check if we can extend current space

    // Allocate new space
    const MemRef space = Alloc(size);
    if (!space.pointer) return space;

    /*if (doCopy) {*/  //TODO: allow realloc without copying
        // Get size of old segment
        const size_t oldsize = GetCapacityFromHeader(p);

        // Copy existing segment
        memcpy(space.pointer, p, oldsize);

        // Add old segment to freelist
        Free(ref, p);
    //}

#ifdef TIGHTDB_DEBUG
    if (m_debugOut) {
        printf("ReAlloc origref: %lu oldsize: %lu newref: %lu newsize: %lu\n", ref, oldsize, space.ref, size);
    }
#endif // TIGHTDB_DEBUG

    return space;
}

void* SlabAlloc::Translate(size_t ref) const
{
    if (ref < m_baseline) return m_shared + ref;
    else {
        const size_t ndx = m_slabs.column().offset.find_pos(ref);
        TIGHTDB_ASSERT(ndx != not_found);

        const size_t offset = ndx ? size_t(m_slabs[ndx-1].offset) : m_baseline;
        return (char*)(intptr_t)m_slabs[ndx].pointer + (ref - offset);
    }
}

bool SlabAlloc::IsReadOnly(size_t ref) const
{
    return ref < m_baseline;
}

bool SlabAlloc::SetSharedBuffer(const char* buffer, size_t len, bool take_ownership)
{
    // Verify that the topref points to a location within buffer.
    // This is currently the only integrity check we make
    size_t ref = (size_t)(*(uint64_t*)buffer);
    if (ref > len) return false;

    // There is a unit test that calls this function with an invalid buffer
    // so we can't size_t-test range with to_ref until now
    ref = to_ref(*(uint64_t*)buffer);
    (void)ref; // the above macro contains an assert, this avoids warning for unused var

    m_shared = (char*)buffer;
    m_baseline = len;
    m_owned = take_ownership; // we now own the buffer
    return true;
}

bool SlabAlloc::SetShared(const char* path, bool read_only)
{
#ifdef _MSC_VER
    TIGHTDB_ASSERT(read_only); // write persistence is not implemented for windows yet
    // Open file
    m_fd = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_ALWAYS, NULL, NULL);

    // Map to memory (read only)
    const HANDLE hMapFile = CreateFileMapping(m_fd, NULL, PAGE_WRITECOPY, 0, 0, 0);
    if (hMapFile == NULL || hMapFile == INVALID_HANDLE_VALUE) {
        CloseHandle(m_fd);
        return false;
    }
    const LPCTSTR pBuf = (LPTSTR) MapViewOfFile(hMapFile, FILE_MAP_COPY, 0, 0, 0);
    if (pBuf == NULL) {
        return false;
    }

    // Get Size
    LARGE_INTEGER size;
    GetFileSizeEx(m_fd, &size);
    m_baseline = to_ref(size.QuadPart);

    m_shared = (char *)pBuf;
    m_mapfile = hMapFile;

    return true;
#else
    // Open file
    {
        m_fd = open(path, read_only ? O_RDONLY : O_RDWR|O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (m_fd < 0) return false;

        // Get size
        struct stat statbuf;
        if (fstat(m_fd, &statbuf) < 0) goto error;
        size_t len = statbuf.st_size;

        // Handle empty files (new database)
        if (len == 0) {
            if (read_only) goto error; // non-existing or empty file

            // We dont want multiple processes creating files at the same time
            if (flock(m_fd, LOCK_EX) != 0) goto error;

            // Verify that file has not been created by other process while
            // we waited for lock
            if (fstat(m_fd, &statbuf) < 0) goto error;
            len = statbuf.st_size;

            if (len == 0) {
                // write file header
                const ssize_t r = write(m_fd, default_header, header_len);
                if (r == -1) goto error;

                // pre-alloc initial space when mmapping
                len = 1024*1024;
                const int r2 = ftruncate(m_fd, len);
                if (r2 == -1) goto error;
            }

            if (flock(m_fd, LOCK_UN) != 0) goto error;
        }

        // Verify that data is 64bit aligned
        if ((len & 0x7) != 0) goto error;

        // Map to memory (read only)
        void* const p = mmap(0, len, PROT_READ, MAP_SHARED, m_fd, 0);
        if (p == (void*)-1) goto error;

        // Verify the data structures
        {
            // File header is 24 bytes, composed of three 64bit
            // blocks. The two first being top_refs (only one valid
            // at a time) and the last being the info block.
            const char* const file_header = (char*)p;

            // First four bytes of info block is file format id
            if (!(file_header[16] == 'T' &&
                  file_header[17] == '-' &&
                  file_header[18] == 'D' &&
                  file_header[19] == 'B'))
                return false; // Not a tightdb file

            // Last bit in info block indicates which top_ref block is valid
            const size_t valid_part = file_header[23] & 0x1;
            if (valid_part != 0 && valid_part != 1)
                return false; // invalid header

            // Byte 4 and 5 (depending on valid_part) in the info block is version
            const uint8_t version = file_header[valid_part ? 21 : 20];
            if (version != 0)
                return false; // unsupported version
        }

        m_shared = static_cast<char*>(p);
        m_baseline = len;

        return true;
    }

error:
    if (m_fd >= 0)
        close(m_fd);
    return false;
#endif
}

bool SlabAlloc::RefreshMapping()
{
#if !defined(_MSC_VER) // write persistence
    // We need a lock on the file so we don't get
    // a partial size because some other process is
    // creating it.
    if (flock(m_fd, LOCK_EX) != 0) return false;

    // Get current file size
    struct stat statbuf;
    if (fstat(m_fd, &statbuf) < 0) return false;
    const size_t len = statbuf.st_size;

    // Remap file if needed
    ReMap(len);

    if (flock(m_fd, LOCK_UN) != 0) return false;
#endif

    return true;
}

bool SlabAlloc::CanPersist() const
{
    return m_shared != NULL;
}

size_t SlabAlloc::GetTopRef() const
{
    TIGHTDB_ASSERT(m_shared && m_baseline > 0);

    // File header is 24 bytes, composed of three 64bit
    // blocks. The two first being top_refs (only one valid
    // at a time) and the last being the info block.
    const char* const file_header = (const char*)m_shared;

    // Last bit in info block indicates which top_ref block
    // is valid
    const size_t valid_ref = file_header[23] & 0x1;

    const uint64_t* const top_refs = (uint64_t*)m_shared;
    const size_t ref = to_ref(top_refs[valid_ref]);
    TIGHTDB_ASSERT(ref < m_baseline);

    return ref;
}

size_t SlabAlloc::GetTotalSize() const
{
    if (m_slabs.is_empty()) {
        return m_baseline;
    }
    else {
        return to_ref(m_slabs.back().offset);
    }
}

void SlabAlloc::FreeAll(size_t filesize)
{
    TIGHTDB_ASSERT(filesize >= m_baseline);
    TIGHTDB_ASSERT((filesize & 0x7) == 0 || filesize == (size_t)-1); // 64bit alignment

    // Free all scratch space (done after all data has
    // been commited to persistent space)
    m_freeReadOnly.clear();
    m_freeSpace.clear();

    // Rebuild free list to include all slabs
    size_t ref = m_baseline;
    const size_t count = m_slabs.size();
    for (size_t i = 0; i < count; ++i) {
        const Slabs::Cursor c = m_slabs[i];
        const size_t size = c.offset - ref;

        m_freeSpace.add(ref, size);

        ref = c.offset;
    }

    // If the file size have changed, we need to remap the readonly buffer
    if (filesize != (size_t)-1)
        ReMap(filesize);

    TIGHTDB_ASSERT(IsAllFree());
}

bool SlabAlloc::ReMap(size_t filesize)
{
    TIGHTDB_ASSERT(m_freeReadOnly.is_empty());
    TIGHTDB_ASSERT(m_slabs.size() == m_freeSpace.size());

    // We only need to remap the readonly buffer
    // if the file size have changed.
    if (filesize == m_baseline) return false;

    TIGHTDB_ASSERT(filesize >= m_baseline);
    TIGHTDB_ASSERT((filesize & 0x7) == 0); // 64bit alignment

#if !defined(_MSC_VER) // write persistence
    //void* const p = mremap(m_shared, m_baseline, filesize); // linux only
    munmap(m_shared, m_baseline);
    void* const p = mmap(0, filesize, PROT_READ, MAP_SHARED, m_fd, 0);
    TIGHTDB_ASSERT(p);

    m_shared   = (char*)p;
    m_baseline = filesize;
#endif

    // Rebase slabs and free list
    size_t new_offset = filesize;
    const size_t count = m_slabs.size();
    for (size_t i = 0; i < count; ++i) {
        FreeSpace::Cursor c = m_freeSpace[i];
        c.ref = new_offset;
        new_offset += c.size;

        m_slabs[i].offset = new_offset;
    }

    return true;
}

#ifdef TIGHTDB_DEBUG

bool SlabAlloc::IsAllFree() const
{
    if (m_freeSpace.size() != m_slabs.size()) return false;

    // Verify that free space matches slabs
    size_t ref = m_baseline;
    for (size_t i = 0; i < m_slabs.size(); ++i) {
        Slabs::ConstCursor c = m_slabs[i];
        const size_t size = to_ref(c.offset) - ref;

        const size_t r = m_freeSpace.column().ref.find_first(ref);
        if (r == (size_t)-1) return false;
        if (size != (size_t)m_freeSpace[r].size) return false;

        ref = to_ref(c.offset);
    }
    return true;
}

void SlabAlloc::Verify() const
{
    // Make sure that all free blocks fit within a slab
    const size_t count = m_freeSpace.size();
    for (size_t i = 0; i < count; ++i) {
        FreeSpace::ConstCursor c = m_freeSpace[i];
        const size_t ref = to_ref(c.ref);

        const size_t ndx = m_slabs.column().offset.find_pos(ref);
        TIGHTDB_ASSERT(ndx != size_t(-1));

        const size_t slab_end = to_ref(m_slabs[ndx].offset);
        const size_t free_end = ref + to_ref(c.size);

        TIGHTDB_ASSERT(free_end <= slab_end);
    }
}

void SlabAlloc::Print() const
{
    const size_t allocated = m_slabs.is_empty() ? 0 : (size_t)m_slabs[m_slabs.size()-1].offset;

    size_t free = 0;
    for (size_t i = 0; i < m_freeSpace.size(); ++i) {
        free += to_ref(m_freeSpace[i].size);
    }

    cout << "Base: " << (m_shared ? m_baseline : 0) << " Allocated: " << (allocated - free) << "\n";
}

#endif // TIGHTDB_DEBUG

} //namespace tightdb
