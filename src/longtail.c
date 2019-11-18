#include "longtail.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "stb_ds.h"

#ifdef LONGTAIL_VERBOSE_LOGS
    #define LONGTAIL_LOG(fmt, ...) \
        printf("--- ");printf(fmt, __VA_ARGS__);
#else
    #define LONGTAIL_LOG(fmr, ...)
#endif

#if !defined(LONGTAIL_ATOMICADD)
    #if defined(__clang__) || defined(__GNUC__)
        #define LONGTAIL_ATOMICADD_PRIVATE(value, amount) (__sync_add_and_fetch (value, amount))
    #elif defined(_MSC_VER)
        #if !defined(_WINDOWS_)
            #define WIN32_LEAN_AND_MEAN
            #include <Windows.h>
            #undef WIN32_LEAN_AND_MEAN
        #endif

        #define LONGTAIL_ATOMICADD_PRIVATE(value, amount) (_InterlockedExchangeAdd((volatile LONG *)value, amount) + amount)
    #else
        inline int32_t LONGTAIL_NonAtomicAdd(volatile int32_t* store, int32_t value) { *store += value; return *store; }
        #define LONGTAIL_ATOMICADD_PRIVATE(value, amount) (LONGTAIL_NonAtomicAdd(value, amount))
    #endif
#else
    #define LONGTAIL_ATOMICADD_PRIVATE LONGTAIL_ATOMICADD
#endif

int IsDirPath(const char* path)
{
    return path[0] ? path[strlen(path) - 1] == '/' : 0;
}

TLongtail_Hash GetPathHash(struct HashAPI* hash_api, const char* path)
{
    HashAPI_HContext context = hash_api->BeginContext(hash_api);
    hash_api->Hash(hash_api, context, (uint32_t)strlen(path), (void*)path);
    return (TLongtail_Hash)hash_api->EndContext(hash_api, context);
}

int SafeCreateDir(struct StorageAPI* storage_api, const char* path)
{
    if (storage_api->CreateDir(storage_api, path))
    {
        return 1;
    }
    if (storage_api->IsDir(storage_api, path))
    {
        return 1;
    }
    return 0;
}

int EnsureParentPathExists(struct StorageAPI* storage_api, const char* path)
{
    char* dir_path = strdup(path);
    char* last_path_delimiter = (char*)strrchr(dir_path, '/');
    if (last_path_delimiter == 0)
    {
        return 1;
    }
    *last_path_delimiter = '\0';
    if (storage_api->IsDir(storage_api, dir_path))
    {
        free(dir_path);
        return 1;
    }
    else
    {
        if (!EnsureParentPathExists(storage_api, dir_path))
        {
            LONGTAIL_LOG("EnsureParentPathExists failed: `%s`\n", dir_path)
            free(dir_path);
            return 0;
        }
        if (SafeCreateDir(storage_api, dir_path))
        {
            free(dir_path);
            return 1;
        }
    }
    LONGTAIL_LOG("EnsureParentPathExists failed: `%s`\n", dir_path)
    free(dir_path);
    return 0;
}

struct HashToIndexItem
{
    TLongtail_Hash key;
    uint32_t value;
};

typedef void (*ProcessEntry)(void* context, const char* root_path, const char* file_name);

int RecurseTree(struct StorageAPI* storage_api, const char* root_folder, ProcessEntry entry_processor, void* context)
{
    LONGTAIL_LOG("RecurseTree `%s`\n", root_folder)

    uint32_t folder_index = 0;

    char** folder_paths = 0;
    arrsetcap(folder_paths, 256);

    arrput(folder_paths, strdup(root_folder));

    while (folder_index < (uint32_t)arrlen(folder_paths))
    {
        const char* asset_folder = folder_paths[folder_index++];

        StorageAPI_HIterator fs_iterator = storage_api->StartFind(storage_api, asset_folder);
        if (fs_iterator)
        {
            do
            {
                const char* dir_name = storage_api->GetDirectoryName(storage_api, fs_iterator);
                if (dir_name)
                {
                    entry_processor(context, asset_folder, dir_name);
                    if (arrlen(folder_paths) == arrcap(folder_paths))
                    {
                        if (folder_index > 0)
                        {
                            uint32_t unprocessed_count = (uint32_t)(arrlen(folder_paths) - folder_index);
                            memmove(folder_paths, &folder_paths[folder_index], sizeof(const char*) * unprocessed_count);
                            arrsetlen(folder_paths, unprocessed_count);
                            folder_index = 0;
                        }
                    }
                    arrput(folder_paths, storage_api->ConcatPath(storage_api, asset_folder, dir_name));
                }
                else
                {
                    const char* file_name = storage_api->GetFileName(storage_api, fs_iterator);
                    if (file_name)
                    {
                        entry_processor(context, asset_folder, file_name);
                    }
                }
            }while(storage_api->FindNext(storage_api, fs_iterator));
            storage_api->CloseFind(storage_api, fs_iterator);
        }
        free((void*)asset_folder);
    }
    arrfree(folder_paths);
    folder_paths = 0;
    return 1;
}

size_t GetPathsSize(uint32_t path_count, uint32_t path_data_size)
{
    return sizeof(struct Paths) +
        sizeof(uint32_t) +                // PathCount
        sizeof(uint32_t) * path_count +    // m_Offsets
        path_data_size;
};

struct Paths* CreatePaths(uint32_t path_count, uint32_t path_data_size)
{
    struct Paths* paths = (struct Paths*)malloc(GetPathsSize(path_count, path_data_size));
    char* p = (char*)&paths[1];
    paths->m_DataSize = 0;
    paths->m_PathCount = (uint32_t*)p;
    p += sizeof(uint32_t);
    paths->m_Offsets = (uint32_t*)p;
    p += sizeof(uint32_t) * path_count;
    paths->m_Data = p;
    *paths->m_PathCount = 0;
    return paths;
};

struct Paths* MakePaths(uint32_t path_count, const char* const* path_names)
{
    uint32_t name_data_size = 0;
    for (uint32_t i = 0; i < path_count; ++i)
    {
        name_data_size += (uint32_t)strlen(path_names[i]) + 1;
    }
    struct Paths* paths = CreatePaths(path_count, name_data_size);
    uint32_t offset = 0;
    for (uint32_t i = 0; i < path_count; ++i)
    {
        uint32_t length = (uint32_t)strlen(path_names[i]) + 1;
        paths->m_Offsets[i] = offset;
        memmove(&paths->m_Data[offset], path_names[i], length);
        offset += length;
    }
    paths->m_DataSize = offset;
    *paths->m_PathCount = path_count;
    return paths;
}

struct Paths* AppendPath(struct Paths* paths, const char* path, uint32_t* max_path_count, uint32_t* max_data_size, uint32_t path_count_increment, uint32_t data_size_increment)
{
    uint32_t path_size = (uint32_t)(strlen(path) + 1);

    int out_of_path_data = paths->m_DataSize + path_size > *max_data_size;
    int out_of_path_count = *paths->m_PathCount >= *max_path_count;
    if (out_of_path_count | out_of_path_data)
    {
        uint32_t extra_path_count = out_of_path_count ? path_count_increment : 0;
        uint32_t extra_path_data_size = out_of_path_data ? (path_count_increment * data_size_increment) : 0;

        const uint32_t new_path_count = *max_path_count + extra_path_count;
        const uint32_t new_path_data_size = *max_data_size + extra_path_data_size;
        struct Paths* new_paths = CreatePaths(new_path_count, new_path_data_size);
        *max_path_count = new_path_count;
        *max_data_size = new_path_data_size;
        new_paths->m_DataSize = paths->m_DataSize;
        *new_paths->m_PathCount = *paths->m_PathCount;

        memmove(new_paths->m_Offsets, paths->m_Offsets, sizeof(uint32_t) * *paths->m_PathCount);
        memmove(new_paths->m_Data, paths->m_Data, paths->m_DataSize);

        free(paths);
        paths = new_paths;
    }

    memmove(&paths->m_Data[paths->m_DataSize], path, path_size);
    paths->m_Offsets[*paths->m_PathCount] = paths->m_DataSize;
    paths->m_DataSize += path_size;
    (*paths->m_PathCount)++;

    return paths;
}

struct AddFile_Context {
    struct StorageAPI* m_StorageAPI;
    uint32_t m_ReservedPathCount;
    uint32_t m_ReservedPathSize;
    uint32_t m_RootPathLength;
    struct Paths* m_Paths;
};

void AddFile(void* context, const char* root_path, const char* file_name)
{
    struct AddFile_Context* paths_context = (struct AddFile_Context*)context;
    struct StorageAPI* storage_api = paths_context->m_StorageAPI;

    char* full_path = storage_api->ConcatPath(storage_api, root_path, file_name);
    if (storage_api->IsDir(storage_api, full_path))
    {
        uint32_t path_length = (uint32_t)strlen(full_path);
        char* full_dir_path = (char*)malloc(path_length + 1 + 1);
        strcpy(full_dir_path, full_path);
        strcpy(&full_dir_path[path_length], "/");
        free(full_path);
        full_path = full_dir_path;
    }

    struct Paths* paths = paths_context->m_Paths;
    const uint32_t root_path_length = paths_context->m_RootPathLength;
    const char* s = &full_path[root_path_length];
    if (*s == '/')
    {
        ++s;
    }

    paths_context->m_Paths = AppendPath(paths_context->m_Paths, s, &paths_context->m_ReservedPathCount, &paths_context->m_ReservedPathSize, 512, 128);

    free(full_path);
    full_path = 0;
}

struct Paths* GetFilesRecursively(struct StorageAPI* storage_api, const char* root_path)
{
    LONGTAIL_LOG("GetFilesRecursively `%s`\n", root_path)
    const uint32_t default_path_count = 512;
    const uint32_t default_path_data_size = default_path_count * 128;

    struct Paths* paths = CreatePaths(default_path_count, default_path_data_size);
    struct AddFile_Context context = {storage_api, default_path_count, default_path_data_size, (uint32_t)(strlen(root_path)), paths};
    paths = 0;

    if(!RecurseTree(storage_api, root_path, AddFile, &context))
    {
        free(context.m_Paths);
        return 0;
    }

    return context.m_Paths;
}

struct HashJob
{
    struct StorageAPI* m_StorageAPI;
    struct HashAPI* m_HashAPI;
    TLongtail_Hash* m_PathHash;
    TLongtail_Hash* m_ContentHash;
    uint32_t* m_ContentSize;
    uint32_t* m_AssetChunkCount;
    const char* m_RootPath;
    const char* m_Path;
    TLongtail_Hash* m_ChunkHashes;
    uint32_t* m_ChunkSizes;
    uint32_t m_MaxChunkSize;
    int m_Success;
};

void HashFile(void* context)
{
    struct HashJob* hash_job = (struct HashJob*)context;

    hash_job->m_Success = 0;

    *hash_job->m_AssetChunkCount = 0;
    *hash_job->m_ContentSize = 0;
    *hash_job->m_ContentHash = 0;

    *hash_job->m_PathHash = GetPathHash(hash_job->m_HashAPI, hash_job->m_Path);

    if (IsDirPath(hash_job->m_Path))
    {
        hash_job->m_Success = 1;
        return;
    }
    uint32_t chunk_count = 0;

    struct StorageAPI* storage_api = hash_job->m_StorageAPI;
    char* path = storage_api->ConcatPath(storage_api, hash_job->m_RootPath, hash_job->m_Path);
    StorageAPI_HOpenFile file_handle = storage_api->OpenReadFile(storage_api, path);
    if (!file_handle)
    {
        free(path);
        return;
    }

    uint32_t asset_size = (uint32_t)storage_api->GetSize(storage_api, file_handle);
    uint8_t* batch_data = (uint8_t*)malloc(hash_job->m_MaxChunkSize);
    uint32_t max_chunks = (asset_size + hash_job->m_MaxChunkSize - 1) / hash_job->m_MaxChunkSize;

    hash_job->m_ChunkHashes = (TLongtail_Hash*)malloc(sizeof(TLongtail_Hash) * max_chunks);
    hash_job->m_ChunkSizes = (uint32_t*)malloc(sizeof(uint32_t) * max_chunks);

    HashAPI_HContext asset_hash_context = hash_job->m_HashAPI->BeginContext(hash_job->m_HashAPI);

    uint32_t offset = 0;
    while (offset != asset_size)
    {
        uint32_t len = (uint32_t)((asset_size - offset) < hash_job->m_MaxChunkSize ? (asset_size - offset) : hash_job->m_MaxChunkSize);
        int read_ok = storage_api->Read(storage_api, file_handle, offset, len, batch_data);
        if (!read_ok)
        {
            LONGTAIL_LOG("HashFile failed to read from `%s`\n", path)
            hash_job->m_Success = 0;
            free(hash_job->m_ChunkSizes);
            hash_job->m_ChunkSizes = 0;
            free(hash_job->m_ChunkHashes);
            hash_job->m_ChunkHashes = 0;
            storage_api->CloseRead(storage_api, file_handle);
            file_handle = 0;
            free(path);
            return;
        }

        {
            HashAPI_HContext chunk_hash_context = hash_job->m_HashAPI->BeginContext(hash_job->m_HashAPI);
            hash_job->m_HashAPI->Hash(hash_job->m_HashAPI, chunk_hash_context, len, batch_data);
            TLongtail_Hash chunk_hash = hash_job->m_HashAPI->EndContext(hash_job->m_HashAPI, chunk_hash_context);
            hash_job->m_ChunkHashes[chunk_count] = chunk_hash;
            hash_job->m_ChunkSizes[chunk_count] = len;
        }
        ++chunk_count;

        offset += len;
        hash_job->m_HashAPI->Hash(hash_job->m_HashAPI, asset_hash_context, len, batch_data);
    }

    TLongtail_Hash content_hash = hash_job->m_HashAPI->EndContext(hash_job->m_HashAPI, asset_hash_context);

    storage_api->CloseRead(storage_api, file_handle);
    file_handle = 0;
    
    *hash_job->m_ContentSize = asset_size;
    *hash_job->m_ContentHash = content_hash;
    *hash_job->m_ContentSize = asset_size;
    *hash_job->m_AssetChunkCount = chunk_count;

    free((char*)path);
    path = 0;

    hash_job->m_Success = 1;
}

int ChunkAssets(
    struct StorageAPI* storage_api,
    struct HashAPI* hash_api,
    struct JobAPI* job_api,
    const char* root_path,
    const struct Paths* paths,
    TLongtail_Hash* path_hashes,
    TLongtail_Hash* content_hashes,
    uint32_t* content_sizes,
    uint32_t* asset_chunk_start_index,
    uint32_t* asset_chunk_counts,
    uint32_t** chunk_sizes,
    TLongtail_Hash** chunk_hashes,
    uint32_t max_chunk_size,
    uint32_t* chunk_count)
{
    LONGTAIL_LOG("ChunkAssets in folder `%s` for %u assets\n", root_path, (uint32_t)*paths->m_PathCount)
    uint32_t asset_count = *paths->m_PathCount;

    if (job_api)
    {
        if (!job_api->ReserveJobs(job_api, asset_count))
        {
            return 0;
        }
    }
    struct HashJob* hash_jobs = (struct HashJob*)malloc(sizeof(struct HashJob) * asset_count);

    uint64_t assets_left = asset_count;
    uint64_t offset = 0;
	for (uint32_t asset_index = 0; asset_index < asset_count; ++asset_index)
    {
		JobAPI_JobFunc func = HashFile;
        struct HashJob* job = &hash_jobs[asset_index];
		void* ctx = &hash_jobs[asset_index];
		job->m_StorageAPI = storage_api;
        job->m_HashAPI = hash_api;
        job->m_RootPath = root_path;
        job->m_Path = &paths->m_Data[paths->m_Offsets[asset_index]];
        job->m_PathHash = &path_hashes[asset_index];
        job->m_ContentHash = &content_hashes[asset_index];
        job->m_ContentSize = &content_sizes[asset_index];
        job->m_AssetChunkCount = &asset_chunk_counts[asset_index];
        job->m_ChunkHashes = 0;
        job->m_ChunkSizes = 0;
        job->m_MaxChunkSize = max_chunk_size;
        if (!job_api)
        {
            HashFile(job);
        }

        if (job_api)
        {
            job_api->SubmitJobs(job_api, 1, &func, &ctx);
        }
    }

    if (job_api)
    {
        job_api->WaitForAllJobs(job_api);
    }

    int success = 1;
    for (uint32_t i = 0; i < asset_count; ++i)
    {
        if (!hash_jobs[i].m_Success)
        {
            success = 0;
            LONGTAIL_LOG("Failed to hash `%s`\n", hash_jobs[i].m_Path)
        }
    }

    if (success)
    {
        uint32_t built_chunk_count = 0;
        for (uint32_t i = 0; i < asset_count; ++i)
        {
            built_chunk_count += asset_chunk_counts[i];
        }
        *chunk_count = built_chunk_count;
        *chunk_sizes = (uint32_t*)malloc(sizeof(uint32_t) * *chunk_count);
        *chunk_hashes = (TLongtail_Hash*)malloc(sizeof(TLongtail_Hash) * *chunk_count);

        uint32_t chunk_offset = 0;
        for (uint32_t i = 0; i < asset_count; ++i)
        {
            asset_chunk_start_index[i] = chunk_offset;
            for (uint32_t chunk_index = 0; chunk_index < asset_chunk_counts[i]; ++chunk_index)
            {
                (*chunk_sizes)[chunk_offset] = hash_jobs[i].m_ChunkSizes[chunk_index];
                (*chunk_hashes)[chunk_offset] = hash_jobs[i].m_ChunkHashes[chunk_index];
                ++chunk_offset;
            }
        }
    }
    else
    {
        *chunk_count = 0;
        *chunk_sizes = 0;
        *chunk_hashes = 0;
    }

    for (uint32_t i = 0; i < asset_count; ++i)
    {
        free(hash_jobs[i].m_ChunkHashes);
        hash_jobs[i].m_ChunkHashes = 0;
        free(hash_jobs[i].m_ChunkSizes);
        hash_jobs[i].m_ChunkSizes = 0;
    }

    free(hash_jobs);
    hash_jobs = 0;

    return success;
}

size_t GetVersionIndexDataSize(
    uint32_t asset_count,
    uint32_t chunk_count,
    uint32_t asset_chunk_index_count,
    uint32_t path_data_size)
{
    size_t version_index_data_size =
        sizeof(uint32_t) +                              // m_AssetCount
        sizeof(uint32_t) +                              // m_ChunkCount
        sizeof(uint32_t) +                              // m_AssetChunkIndexCount
        (sizeof(TLongtail_Hash) * asset_count) +        // m_PathHashes
        (sizeof(TLongtail_Hash) * asset_count) +        // m_ContentHashes
        (sizeof(uint32_t) * asset_count) +              // m_AssetSizes
        (sizeof(uint32_t) * asset_count) +              // m_AssetChunkCounts
        (sizeof(uint32_t) * asset_count) +              // m_AssetChunkIndexStarts
        (sizeof(uint32_t) * asset_chunk_index_count) +  // m_AssetChunkIndexes
        (sizeof(TLongtail_Hash) * chunk_count) +        // m_ChunkHashes
        (sizeof(uint32_t) * chunk_count) +              // m_ChunkSizes
        (sizeof(uint32_t) * asset_count) +              // m_NameOffsets
        path_data_size;

    return version_index_data_size;
}

size_t GetVersionIndexSize(
    uint32_t asset_count,
    uint32_t chunk_count,
    uint32_t asset_chunk_index_count,
    uint32_t path_data_size)
{
    return sizeof(struct VersionIndex) +
            GetVersionIndexDataSize(asset_count, chunk_count, asset_chunk_index_count, path_data_size);
}

void InitVersionIndex(struct VersionIndex* version_index, size_t version_index_data_size)
{
    char* p = (char*)version_index;
    p += sizeof(struct VersionIndex);

    size_t version_index_data_start = (size_t)p;

    version_index->m_AssetCount = (uint32_t*)p;
    p += sizeof(uint32_t);

    uint32_t asset_count = *version_index->m_AssetCount;

    version_index->m_ChunkCount = (uint32_t*)p;
    p += sizeof(uint32_t);

    uint32_t chunk_count = *version_index->m_ChunkCount;

    version_index->m_AssetChunkIndexCount = (uint32_t*)p;
    p += sizeof(uint32_t);

    uint32_t asset_chunk_index_count = *version_index->m_AssetChunkIndexCount;

    version_index->m_PathHashes = (TLongtail_Hash*)p;
    p += (sizeof(TLongtail_Hash) * asset_count);

    version_index->m_ContentHashes = (TLongtail_Hash*)p;
    p += (sizeof(TLongtail_Hash) * asset_count);

    version_index->m_AssetSizes = (uint32_t*)p;
    p += (sizeof(uint32_t) * asset_count);

    version_index->m_AssetChunkCounts = (uint32_t*)p;
    p += (sizeof(uint32_t) * asset_count);

    version_index->m_AssetChunkIndexStarts = (uint32_t*)p;
    p += (sizeof(uint32_t) * asset_count);

    version_index->m_AssetChunkIndexes = (uint32_t*)p;
    p += (sizeof(uint32_t) * asset_chunk_index_count);

    version_index->m_ChunkHashes = (TLongtail_Hash*)p;
    p += (sizeof(TLongtail_Hash) * chunk_count);

    version_index->m_ChunkSizes = (uint32_t*)p;
    p += (sizeof(uint32_t) * chunk_count);

    version_index->m_NameOffsets = (uint32_t*)p;
    p += (sizeof(uint32_t) * asset_count);

    size_t version_index_name_data_start = (size_t)p;

    version_index->m_NameDataSize = (uint32_t)(version_index_data_size - (version_index_name_data_start - version_index_data_start));

    version_index->m_NameData = (char*)p;
}

struct VersionIndex* BuildVersionIndex(
    void* mem,
    size_t mem_size,
    const struct Paths* paths,
    const TLongtail_Hash* path_hashes,
    const TLongtail_Hash* content_hashes,
    const uint32_t* content_sizes,
    const uint32_t* asset_chunk_start_index,
    const uint32_t* asset_chunk_counts,
    const uint32_t* asset_chunk_index_starts,
    uint32_t asset_chunk_index_count,
    const uint32_t* asset_chunk_indexes,
    uint32_t chunk_count,
    const uint32_t* chunk_sizes,
    const TLongtail_Hash* chunk_hashes)
{
    uint32_t asset_count = *paths->m_PathCount;
    struct VersionIndex* version_index = (struct VersionIndex*)mem;
    version_index->m_AssetCount = (uint32_t*)&((char*)mem)[sizeof(struct VersionIndex)];
    version_index->m_ChunkCount = (uint32_t*)&((char*)mem)[sizeof(struct VersionIndex) + sizeof(uint32_t)];
    version_index->m_AssetChunkIndexCount = (uint32_t*)&((char*)mem)[sizeof(struct VersionIndex) + sizeof(uint32_t) + sizeof(uint32_t)];
    *version_index->m_AssetCount = asset_count;
    *version_index->m_ChunkCount = chunk_count;
    *version_index->m_AssetChunkIndexCount = asset_chunk_index_count;

    InitVersionIndex(version_index, mem_size - sizeof(struct VersionIndex));

    memmove(version_index->m_PathHashes, path_hashes, sizeof(TLongtail_Hash) * asset_count);
    memmove(version_index->m_ContentHashes, content_hashes, sizeof(TLongtail_Hash) * asset_count);
    memmove(version_index->m_AssetSizes, content_sizes, sizeof(uint32_t) * asset_count);
    memmove(version_index->m_NameOffsets, paths, sizeof(uint32_t) * asset_count);
    memmove(version_index->m_AssetChunkCounts, asset_chunk_counts, sizeof(uint32_t) * asset_count);
    memmove(version_index->m_AssetChunkIndexStarts, asset_chunk_index_starts, sizeof(uint32_t) * asset_count);
    memmove(version_index->m_AssetChunkIndexes, asset_chunk_indexes, sizeof(uint32_t) * asset_chunk_index_count);
    memmove(version_index->m_ChunkHashes, chunk_hashes, sizeof(TLongtail_Hash) * chunk_count);
    memmove(version_index->m_ChunkSizes, chunk_sizes, sizeof(uint32_t) * chunk_count);
    memmove(version_index->m_NameOffsets, paths->m_Offsets, sizeof(uint32_t) * asset_count);
    memmove(version_index->m_NameData, paths->m_Data, paths->m_DataSize);

    return version_index;
}

struct VersionIndex* CreateVersionIndex(
    struct StorageAPI* storage_api,
    struct HashAPI* hash_api,
    struct JobAPI* job_api,
    const char* root_path,
    const struct Paths* paths,
    uint32_t max_chunk_size)
{
    uint32_t path_count = *paths->m_PathCount;
    uint32_t* content_sizes = (uint32_t*)malloc(sizeof(uint32_t) * path_count);
    TLongtail_Hash* path_hashes = (TLongtail_Hash*)malloc(sizeof(TLongtail_Hash) * path_count);
    TLongtail_Hash* content_hashes = (TLongtail_Hash*)malloc(sizeof(TLongtail_Hash) * path_count);
    uint32_t* asset_chunk_counts = (uint32_t*)malloc(sizeof(uint32_t) * path_count);

    uint32_t assets_chunk_index_count = 0;
    uint32_t* asset_chunk_sizes = 0;
    TLongtail_Hash* asset_chunk_hashes = 0;
    uint32_t* asset_chunk_start_index = (uint32_t*)malloc(sizeof(uint32_t) * path_count);
    
    if (!ChunkAssets(
        storage_api,
        hash_api,
        job_api,
        root_path,
        paths,
        path_hashes,
        content_hashes,
        content_sizes,
        asset_chunk_start_index,
        asset_chunk_counts,
        &asset_chunk_sizes,
        &asset_chunk_hashes,
        max_chunk_size,
        &assets_chunk_index_count))
    {
        LONGTAIL_LOG("Failed to hash assets in `%s`\n", root_path);
        free(asset_chunk_start_index);
        free(asset_chunk_hashes);
        free(asset_chunk_sizes);
        free(content_hashes);
        free(path_hashes);
        free(content_sizes);
        return 0;
    }

    uint32_t* asset_chunk_indexes = (uint32_t*)malloc(sizeof(uint32_t) * assets_chunk_index_count);
    TLongtail_Hash* compact_chunk_hashes = (TLongtail_Hash*)malloc(sizeof(TLongtail_Hash) * assets_chunk_index_count);
    uint32_t* compact_chunk_sizes =  (uint32_t*)malloc(sizeof(uint32_t) * assets_chunk_index_count);

    uint32_t unique_chunk_count = 0;
    struct HashToIndexItem* chunk_hash_to_index = 0;
    for (uint32_t c = 0; c < assets_chunk_index_count; ++c)
    {
        TLongtail_Hash h = asset_chunk_hashes[c];
        intptr_t i = hmgeti(chunk_hash_to_index, h);
        if (i == -1)
        {
            i = unique_chunk_count;
            hmput(chunk_hash_to_index, h, unique_chunk_count);
            compact_chunk_hashes[unique_chunk_count] = h;
            compact_chunk_sizes[unique_chunk_count] = asset_chunk_sizes[c];;
            ++unique_chunk_count;
        }
        asset_chunk_indexes[c] = i;
    }

    hmfree(chunk_hash_to_index);
    chunk_hash_to_index = 0;

    uint32_t* asset_chunk_index_starts = (uint32_t*)malloc(sizeof(uint32_t) * path_count);
    uint32_t asset_chunk_index_start_offset = 0;
    for (uint32_t asset_index = 0; asset_index < path_count; ++asset_index)
    {
        asset_chunk_index_starts[asset_index] = asset_chunk_index_start_offset;
        asset_chunk_index_start_offset += asset_chunk_counts[asset_index];
    }

/*
    TLongtail_Hash* unique_chunk_hashes = (TLongtail_Hash*)malloc(sizeof(TLongtail_Hash) * chunk_count);
    uint32_t* unique_chunk_sizes = (uint32_t*)malloc(sizeof(uint32_t) * chunk_count);
    // Get unique chunks!
    uint32_t unique_chunk_count = 0;
    struct HashToIndexItem* chunk_lookup = 0;
    for (uint32_t c = 0; c < chunk_count; ++c)
    {
        TLongtail_Hash h = chunk_hashes[c];
        uint32_t s = chunk_sizes[c];
        intptr_t i = hmgeti(chunk_lookup, h);
        if (i != -1)
        {
            continue;
        }
        hmput(chunk_lookup, h, unique_chunk_count);
        unique_chunk_hashes[unique_chunk_count] = h;
        unique_chunk_sizes[unique_chunk_count] = s;
        ++unique_chunk_count;
    }
    hmfree(chunk_lookup);
    chunk_lookup = 0;

    free(chunk_sizes);
    chunk_sizes = 0;
    free(chunk_hashes);
    chunk_hashes = 0;
*/
    size_t version_index_size = GetVersionIndexSize(path_count, unique_chunk_count, assets_chunk_index_count, paths->m_DataSize);
    void* version_index_mem = malloc(version_index_size);

    struct VersionIndex* version_index = BuildVersionIndex(
        version_index_mem,
        version_index_size,
        paths,
        path_hashes,
        content_hashes,
        content_sizes,
        asset_chunk_start_index,
        asset_chunk_counts,
        asset_chunk_index_starts,
        assets_chunk_index_count,
        asset_chunk_indexes,
        unique_chunk_count,
        compact_chunk_sizes,
        compact_chunk_hashes);

    free(asset_chunk_index_starts);
    asset_chunk_index_starts = 0;
    free(compact_chunk_sizes);
    compact_chunk_sizes = 0;
    free(compact_chunk_hashes);
    compact_chunk_hashes = 0;
    free(asset_chunk_indexes);
    asset_chunk_indexes = 0;
    free(asset_chunk_sizes);
    asset_chunk_sizes = 0;
    free(asset_chunk_hashes);
    asset_chunk_hashes = 0;
    free(asset_chunk_start_index);
    asset_chunk_start_index = 0;
    free(asset_chunk_counts);
    asset_chunk_counts = 0;
    free(content_hashes);
    content_hashes = 0;
    free(path_hashes);
    path_hashes = 0;
    free(content_sizes);
    content_sizes = 0;

    return version_index;
}

int WriteVersionIndex(struct StorageAPI* storage_api, struct VersionIndex* version_index, const char* path)
{
    LONGTAIL_LOG("WriteVersionIndex to `%s`\n", path)
    size_t index_data_size = GetVersionIndexDataSize((uint32_t)(*version_index->m_AssetCount), (uint32_t)(*version_index->m_ChunkCount), (uint32_t)(*version_index->m_AssetChunkIndexCount), version_index->m_NameDataSize);

    if (!EnsureParentPathExists(storage_api, path))
    {
        return 0;
    }
    StorageAPI_HOpenFile file_handle = storage_api->OpenWriteFile(storage_api, path);
    if (!file_handle)
    {
        return 0;
    }
    if (!storage_api->Write(storage_api, file_handle, 0, index_data_size, &version_index[1]))
    {
        storage_api->CloseWrite(storage_api, file_handle);
        return 0;
    }
    storage_api->CloseWrite(storage_api, file_handle);

    return 1;
}

struct VersionIndex* ReadVersionIndex(struct StorageAPI* storage_api, const char* path)
{
    LONGTAIL_LOG("ReadVersionIndex from `%s`\n", path)
    StorageAPI_HOpenFile file_handle = storage_api->OpenReadFile(storage_api, path);
    if (!file_handle)
    {
        return 0;
    }
    size_t version_index_data_size = storage_api->GetSize(storage_api, file_handle);
    struct VersionIndex* version_index = (struct VersionIndex*)malloc(sizeof(struct VersionIndex) + version_index_data_size);
    if (!version_index)
    {
        storage_api->CloseRead(storage_api, file_handle);
        return 0;
    }
    if (!storage_api->Read(storage_api, file_handle, 0, version_index_data_size, &version_index[1]))
    {
        storage_api->CloseRead(storage_api, file_handle);
        return 0;
    }
    InitVersionIndex(version_index, version_index_data_size);
    storage_api->CloseRead(storage_api, file_handle);
    return version_index;
}

struct BlockIndex
{
    TLongtail_Hash m_BlockHash;
    TLongtail_Hash* m_ChunkHashes; //[]
    uint32_t* m_ChunkSizes; // []
    uint32_t* m_ChunkCount;
};

size_t GetBlockIndexDataSize(uint32_t chunk_count)
{
    return
        (sizeof(TLongtail_Hash) * chunk_count) +    // m_ChunkHashes
        (sizeof(uint32_t) * chunk_count) +          // m_ChunkSizes
        sizeof(uint32_t);                           // m_ChunkCount
}

struct BlockIndex* InitBlockIndex(void* mem, uint32_t asset_count)
{
    struct BlockIndex* block_index = (struct BlockIndex*)mem;
    char* p = (char*)&block_index[1];
    block_index->m_ChunkHashes = (TLongtail_Hash*)p;
    p += sizeof(TLongtail_Hash) * asset_count;
    block_index->m_ChunkSizes = (uint32_t*)p;
    p += sizeof(uint32_t) * asset_count;
    block_index->m_ChunkCount = (uint32_t*)p;
    return block_index;
}

size_t GetBlockIndexSize(uint32_t chunk_count)
{
    size_t block_index_size =
        sizeof(struct BlockIndex) +
        GetBlockIndexDataSize(chunk_count);

    return block_index_size;
}

struct BlockIndex* CreateBlockIndex(
    void* mem,
    struct HashAPI* hash_api,
    uint32_t chunk_count_in_block,
    uint32_t* chunk_indexes,
    const TLongtail_Hash* chunk_hashes,
    const uint32_t* chunk_sizes)
{
    struct BlockIndex* block_index = InitBlockIndex(mem, chunk_count_in_block);
    for (uint32_t i = 0; i < chunk_count_in_block; ++i)
    {
        uint32_t asset_index = chunk_indexes[i];
        block_index->m_ChunkHashes[i] = chunk_hashes[asset_index];
        block_index->m_ChunkSizes[i] = chunk_sizes[asset_index];
    }
    *block_index->m_ChunkCount = chunk_count_in_block;
    HashAPI_HContext hash_context = hash_api->BeginContext(hash_api);
    hash_api->Hash(hash_api, hash_context, (uint32_t)(GetBlockIndexDataSize(chunk_count_in_block)), (void*)&block_index[1]);
    TLongtail_Hash block_hash = hash_api->EndContext(hash_api, hash_context);
    block_index->m_BlockHash = block_hash;

    return block_index;
}

size_t GetContentIndexDataSize(uint64_t block_count, uint64_t asset_count)
{
    size_t block_index_data_size = sizeof(uint64_t) +
        sizeof(uint64_t) +
        (sizeof(TLongtail_Hash) * block_count) +
        (sizeof(TLongtail_Hash) * asset_count) +
        (sizeof(TLongtail_Hash) * asset_count) +
        (sizeof(uint32_t) * asset_count) +
        (sizeof(uint32_t) * asset_count);

    return block_index_data_size;
}

size_t GetContentIndexSize(uint64_t block_count, uint64_t asset_count)
{
    return sizeof(struct ContentIndex) +
        GetContentIndexDataSize(block_count, asset_count);
}

void InitContentIndex(struct ContentIndex* content_index)
{
    char* p = (char*)&content_index[1];
    content_index->m_BlockCount = (uint64_t*)p;
    p += sizeof(uint64_t);
    content_index->m_ChunkCount = (uint64_t*)p;
    p += sizeof(uint64_t);

    uint64_t block_count = *content_index->m_BlockCount;
    uint64_t asset_count = *content_index->m_ChunkCount;

    content_index->m_BlockHashes = (TLongtail_Hash*)p;
    p += (sizeof(TLongtail_Hash) * block_count);
    content_index->m_ChunkHashes = (TLongtail_Hash*)p;
    p += (sizeof(TLongtail_Hash) * asset_count);
    content_index->m_ChunkBlockIndexes = (uint64_t*)p;
    p += (sizeof(uint64_t) * asset_count);
    content_index->m_ChunkBlockOffsets = (uint32_t*)p;
    p += (sizeof(uint32_t) * asset_count);
    content_index->m_ChunkLengths = (uint32_t*)p;
    p += (sizeof(uint32_t) * asset_count);
}

uint32_t GetUniqueHashes(uint64_t asset_count, const TLongtail_Hash* hashes, uint32_t* out_unique_hash_indexes)
{
    struct HashToIndexItem* lookup_table = 0;

    uint32_t unique_asset_count = 0;
    for (uint32_t i = 0; i < asset_count; ++i)
    {
        TLongtail_Hash hash = hashes[i];
        ptrdiff_t lookup_index = hmgeti(lookup_table, hash);
        if (lookup_index == -1)
        {
            hmput(lookup_table, hash, 1);
            out_unique_hash_indexes[unique_asset_count] = i;
            ++unique_asset_count;
        }
        else
        {
            ++lookup_table[lookup_index].value;
        }
    }
    hmfree(lookup_table);
    lookup_table = 0;
    return unique_asset_count;
}

struct ContentIndex* CreateContentIndex(
    struct HashAPI* hash_api,
    uint64_t chunk_count,
    const TLongtail_Hash* chunk_hashes,
    const uint32_t* chunk_sizes,
    uint32_t max_block_size,
    uint32_t max_chunks_per_block)
{
    LONGTAIL_LOG("CreateContentIndex\n")
    if (chunk_count == 0)
    {
        size_t content_index_size = GetContentIndexSize(0, 0);
        struct ContentIndex* content_index = (struct ContentIndex*)malloc(content_index_size);

        content_index->m_BlockCount = (uint64_t*)&((char*)content_index)[sizeof(struct ContentIndex)];
        content_index->m_ChunkCount = (uint64_t*)&((char*)content_index)[sizeof(struct ContentIndex) + sizeof(uint64_t)];
        *content_index->m_BlockCount = 0;
        *content_index->m_ChunkCount = 0;
        InitContentIndex(content_index);
        return content_index;
    }
    uint32_t* chunk_indexes = (uint32_t*)malloc(sizeof(uint32_t) * chunk_count);
    uint32_t unique_chunk_count = GetUniqueHashes(chunk_count, chunk_hashes, chunk_indexes);

    struct BlockIndex** block_indexes = (struct BlockIndex**)malloc(sizeof(struct BlockIndex*) * unique_chunk_count);

    #define MAX_ASSETS_PER_BLOCK 16384u
    uint32_t* stored_chunk_indexes = (uint32_t*)malloc(sizeof(uint32_t) * max_chunks_per_block);

    uint32_t current_size = 0;
    uint64_t i = 0;
    uint32_t asset_count_in_block = 0;
    uint32_t block_count = 0;

    while (i < unique_chunk_count)
    {
        asset_count_in_block = 0;

        uint64_t chunk_index = chunk_indexes[i];

        uint32_t current_size = chunk_sizes[chunk_index];

        stored_chunk_indexes[asset_count_in_block] = chunk_index;
        ++asset_count_in_block;

        while((i + 1) < unique_chunk_count)
        {
            chunk_index = chunk_indexes[(i + 1)];
            uint32_t asset_size = chunk_sizes[chunk_index];

            // Break if resulting asset count will exceed MAX_ASSETS_PER_BLOCK
            if (asset_count_in_block == max_chunks_per_block)
            {
                break;
            }

            // Overshoot by 10% is ok
            if ((current_size + asset_size) > (max_block_size + (max_block_size / 10)))
            {
                break;
            }

            current_size += asset_size;
            stored_chunk_indexes[asset_count_in_block] = chunk_index;
            ++asset_count_in_block;

            ++i;
        }

        block_indexes[block_count] = CreateBlockIndex(
            malloc(GetBlockIndexSize(asset_count_in_block)),
            hash_api,
            asset_count_in_block,
            stored_chunk_indexes,
            chunk_hashes,
            chunk_sizes);

        ++block_count;
        ++i;
    }

    if (current_size > 0)
    {
        block_indexes[block_count] = CreateBlockIndex(
            malloc(GetBlockIndexSize(asset_count_in_block)),
            hash_api,
            asset_count_in_block,
            stored_chunk_indexes,
            chunk_hashes,
            chunk_sizes);
        ++block_count;
    }

    free(stored_chunk_indexes);
    stored_chunk_indexes = 0;
    free(chunk_indexes);
    chunk_indexes = 0;

    // Build Content Index (from block list)
    size_t content_index_size = GetContentIndexSize(block_count, unique_chunk_count);
    struct ContentIndex* content_index = (struct ContentIndex*)malloc(content_index_size);

    content_index->m_BlockCount = (uint64_t*)&((char*)content_index)[sizeof(struct ContentIndex)];
    content_index->m_ChunkCount = (uint64_t*)&((char*)content_index)[sizeof(struct ContentIndex) + sizeof(uint64_t)];
    *content_index->m_BlockCount = block_count;
    *content_index->m_ChunkCount = unique_chunk_count;
    InitContentIndex(content_index);

    uint64_t asset_index = 0;
    for (uint32_t i = 0; i < block_count; ++i)
    {
        struct BlockIndex* block_index = block_indexes[i];
        content_index->m_BlockHashes[i] = block_index->m_BlockHash;
        uint64_t asset_offset = 0;
        for (uint32_t a = 0; a < *block_index->m_ChunkCount; ++a)
        {
            content_index->m_ChunkHashes[asset_index] = block_index->m_ChunkHashes[a];
            content_index->m_ChunkBlockIndexes[asset_index] = i;
            content_index->m_ChunkBlockOffsets[asset_index] = asset_offset;
            content_index->m_ChunkLengths[asset_index] = block_index->m_ChunkSizes[a];

            asset_offset += block_index->m_ChunkSizes[a];
            ++asset_index;
            if (asset_index > unique_chunk_count)
            {
                break;
            }
        }
        free(block_index);
        block_index = 0;
    }
    free(block_indexes);
    block_indexes = 0;

    return content_index;
}

int WriteContentIndex(struct StorageAPI* storage_api, struct ContentIndex* content_index, const char* path)
{
    LONGTAIL_LOG("WriteContentIndex to `%s`, assets %u, blocks %u\n", path, (uint32_t)*content_index->m_ChunkCount, (uint32_t)*content_index->m_BlockCount)
    size_t index_data_size = GetContentIndexDataSize(*content_index->m_BlockCount, *content_index->m_ChunkCount);

    if (!EnsureParentPathExists(storage_api, path))
    {
        return 0;
    }
    StorageAPI_HOpenFile file_handle = storage_api->OpenWriteFile(storage_api, path);
    if (!file_handle)
    {
        return 0;
    }
    if (!storage_api->Write(storage_api, file_handle, 0, index_data_size, &content_index[1]))
    {
        return 0;
    }
    storage_api->CloseWrite(storage_api, file_handle);

    return 1;
}

struct ContentIndex* ReadContentIndex(struct StorageAPI* storage_api, const char* path)
{
    LONGTAIL_LOG("ReadContentIndex from `%s`\n", path)
    StorageAPI_HOpenFile file_handle = storage_api->OpenReadFile(storage_api, path);
    if (!file_handle)
    {
        return 0;
    }
    size_t content_index_data_size = storage_api->GetSize(storage_api, file_handle);
    struct ContentIndex* content_index = (struct ContentIndex*)malloc(sizeof(struct ContentIndex) + content_index_data_size);
    if (!content_index)
    {
        storage_api->CloseRead(storage_api, file_handle);
        return 0;
    }
    if (!storage_api->Read(storage_api, file_handle, 0, content_index_data_size, &content_index[1]))
    {
        storage_api->CloseRead(storage_api, file_handle);
        return 0;
    }
    InitContentIndex(content_index);
    storage_api->CloseRead(storage_api, file_handle);
    return content_index;
}

struct PathLookup
{
    struct HashToIndexItem* m_HashToNameOffset;
    const char* m_NameData;
};

struct PathLookup* CreateContentHashToPathLookup(const struct VersionIndex* version_index, uint64_t* out_unique_asset_indexes)
{
    uint32_t asset_count = (uint32_t)(*version_index->m_AssetCount);
    struct PathLookup* path_lookup = (struct PathLookup*)malloc(sizeof(struct PathLookup));
    path_lookup->m_HashToNameOffset = 0;
    path_lookup->m_NameData = version_index->m_NameData;

    // Only pick up unique assets
    uint32_t unique_asset_count = 0;
    for (uint32_t i = 0; i < asset_count; ++i)
    {
        TLongtail_Hash content_hash = version_index->m_ContentHashes[i];
        ptrdiff_t lookup_index = hmgeti(path_lookup->m_HashToNameOffset, content_hash);
        if (lookup_index == -1)
        {
            hmput(path_lookup->m_HashToNameOffset, content_hash, version_index->m_NameOffsets[i]);
            if (out_unique_asset_indexes)
            {
                out_unique_asset_indexes[unique_asset_count] = i;
            }
            ++unique_asset_count;
        }
    }
    return path_lookup;
}

const char* GetPathFromAssetContentHash(const struct PathLookup* path_lookup, TLongtail_Hash asset_content_hash)
{
    struct HashToIndexItem* lookup = (struct HashToIndexItem*)path_lookup->m_HashToNameOffset;
    ptrdiff_t lookup_index = hmgeti(lookup, asset_content_hash);
    if (lookup_index == -1)
    {
        return 0;
    }
    uint32_t offset = lookup[lookup_index].value;
    return &path_lookup->m_NameData[offset];
}

void FreePathLookup(struct PathLookup* path_lookup)
{
    if (!path_lookup)
    {
        return;
    }
    hmfree(path_lookup->m_HashToNameOffset);
    free(path_lookup);
}

struct AssetPart
{
    const char* m_Path;
    uint64_t m_Start;
};

struct ChunkHashToAssetPart
{
    TLongtail_Hash key;
    struct AssetPart value;
};

struct ChunkHashToAssetPart* CreateAssetPartLookup(
    struct VersionIndex* version_index)
{
    struct ChunkHashToAssetPart* asset_part_lookup = 0;
    for (uint64_t asset_index = 0; asset_index < *version_index->m_AssetCount; ++asset_index)
    {
        const char* path = &version_index->m_NameData[version_index->m_NameOffsets[asset_index]];
        uint64_t asset_chunk_count = version_index->m_AssetChunkCounts[asset_index];
        uint32_t asset_chunk_index_start = version_index->m_AssetChunkIndexStarts[asset_index];
        uint64_t asset_chunk_offset = 0;
        for (uint32_t asset_chunk_index = 0; asset_chunk_index < asset_chunk_count; ++asset_chunk_index)
        {
            uint32_t chunk_index = version_index->m_AssetChunkIndexes[asset_chunk_index_start + asset_chunk_index];
            uint32_t chunk_size = version_index->m_ChunkSizes[chunk_index];
            TLongtail_Hash chunk_hash = version_index->m_ChunkHashes[chunk_index];
            if (hmgeti(asset_part_lookup, chunk_hash) == -1)
            {
                struct AssetPart asset_part = { path, asset_chunk_offset };
                hmput(asset_part_lookup, chunk_hash, asset_part);
            }

            asset_chunk_offset += chunk_size;
        }
    }
    return asset_part_lookup;
}

void FreeAssetPartLookup(struct ChunkHashToAssetPart* asset_part_lookup)
{
    hmfree(asset_part_lookup);
}

struct WriteBlockJob
{
    struct StorageAPI* m_SourceStorageAPI;
    struct StorageAPI* m_TargetStorageAPI;
    struct CompressionAPI* m_CompressionAPI;
    const char* m_ContentFolder;
    const char* m_AssetsFolder;
    const struct ContentIndex* m_ContentIndex;
    struct ChunkHashToAssetPart* m_AssetPartLookup;
    uint64_t m_FirstChunkIndex;
    uint32_t m_ChunkCount;
    uint32_t m_Success;
};

struct WriteBlockJob* CreateWriteContentBlockJob()
{
    size_t job_size = sizeof(struct WriteBlockJob);
    struct WriteBlockJob* job = (struct WriteBlockJob*)malloc(job_size);
    return job;
}

char* GetBlockName(TLongtail_Hash block_hash)
{
    char* name = (char*)malloc(64);
    sprintf(name, "0x%" PRIx64, block_hash);
    return name;
}

void WriteContentBlockJob(void* context)
{
    struct WriteBlockJob* job = (struct WriteBlockJob*)context;
    struct StorageAPI* source_storage_api = job->m_SourceStorageAPI;
    struct StorageAPI* target_storage_api = job->m_TargetStorageAPI;
    struct CompressionAPI* compression_api = job->m_CompressionAPI;

    const struct ContentIndex* content_index = job->m_ContentIndex;
    const char* content_folder = job->m_ContentFolder;
    uint64_t first_chunk_index = job->m_FirstChunkIndex;
    uint32_t chunk_count = job->m_ChunkCount;
    uint64_t block_index = content_index->m_ChunkBlockIndexes[first_chunk_index];
    TLongtail_Hash block_hash = content_index->m_BlockHashes[block_index];

    char* block_name = GetBlockName(block_hash);
    char file_name[64];
    sprintf(file_name, "%s.lrb", block_name);
    char* block_path = target_storage_api->ConcatPath(target_storage_api, content_folder, file_name);

    char tmp_block_name[64];
    sprintf(tmp_block_name, "%s.tmp", block_name);
    char* tmp_block_path = (char*)target_storage_api->ConcatPath(target_storage_api, content_folder, tmp_block_name);

    uint32_t block_data_size = 0;
    for (uint32_t chunk_index = first_chunk_index; chunk_index < first_chunk_index + chunk_count; ++chunk_index)
    {
        uint32_t chunk_size = content_index->m_ChunkLengths[chunk_index];
        block_data_size += chunk_size;
    }

    char* write_buffer = (char*)malloc(block_data_size);
    char* write_ptr = write_buffer;

    for (uint32_t chunk_index = first_chunk_index; chunk_index < first_chunk_index + chunk_count; ++chunk_index)
    {
        TLongtail_Hash chunk_hash = content_index->m_ChunkHashes[chunk_index];
        uint32_t chunk_size = content_index->m_ChunkLengths[chunk_index];    // TODO: We get mismatch on chunk size vs size on disk
        intptr_t asset_part_index = hmgeti(job->m_AssetPartLookup, chunk_hash);
        if (asset_part_index == -1)
        {
            LONGTAIL_LOG("Failed to get path for asset content 0x%" PRIx64 "\n", chunk_hash)
            free(write_buffer);
            free(block_name);
            return;
        }
        const char* asset_path = job->m_AssetPartLookup[asset_part_index].value.m_Path;
        uint64_t asset_offset = job->m_AssetPartLookup[asset_part_index].value.m_Start;

        if (IsDirPath(asset_path))
        {
            LONGTAIL_LOG("Directory should not have any chunks `%s`\n", asset_path)
            free(write_buffer);
            free(block_name);
            return;
        }

        char* full_path = source_storage_api->ConcatPath(source_storage_api, job->m_AssetsFolder, asset_path);
        uint64_t asset_content_offset = job->m_AssetPartLookup[asset_part_index].value.m_Start;
        StorageAPI_HOpenFile file_handle = source_storage_api->OpenReadFile(source_storage_api, full_path);
        if (!file_handle || (source_storage_api->GetSize(source_storage_api, file_handle) < (asset_offset + chunk_size)))
        {
            LONGTAIL_LOG("Missing or mismatching asset content `%s`\n", asset_path)
            free(write_buffer);
            free(block_name);
            source_storage_api->CloseRead(source_storage_api, file_handle);
            return;
        }
        source_storage_api->Read(source_storage_api, file_handle, asset_offset, chunk_size, write_ptr);
        write_ptr += chunk_size;

        source_storage_api->CloseRead(source_storage_api, file_handle);
        free((char*)full_path);
        full_path = 0;
    }

    CompressionAPI_HCompressionContext compression_context = compression_api->CreateCompressionContext(compression_api, compression_api->GetDefaultSettings(compression_api));
    const size_t max_dst_size = compression_api->GetMaxCompressedSize(compression_api, compression_context, block_data_size);
    char* compressed_buffer = (char*)malloc((sizeof(uint32_t) * 2) + max_dst_size);
    ((uint32_t*)compressed_buffer)[0] = (uint32_t)block_data_size;

    size_t compressed_size = compression_api->Compress(compression_api, compression_context, (const char*)write_buffer, &((char*)compressed_buffer)[sizeof(int32_t) * 2], block_data_size, max_dst_size);
    compression_api->DeleteCompressionContext(compression_api, compression_context);
    free(write_buffer);
    if (compressed_size > 0)
    {
        ((uint32_t*)compressed_buffer)[1] = (uint32_t)compressed_size;

        if (!EnsureParentPathExists(target_storage_api, tmp_block_path))
        {
            LONGTAIL_LOG("Failed to create parent path for `%s`\n", tmp_block_path)
            free(compressed_buffer);
            return;
        }
        StorageAPI_HOpenFile block_file_handle = target_storage_api->OpenWriteFile(target_storage_api, tmp_block_path);
        if (!block_file_handle)
        {
            LONGTAIL_LOG("Failed to create block file `%s`\n", tmp_block_path)
            free(compressed_buffer);
            return;
        }
        int write_ok = target_storage_api->Write(target_storage_api, block_file_handle, 0, (sizeof(uint32_t) * 2) + compressed_size, compressed_buffer);
        free(compressed_buffer);
        uint64_t write_offset = (sizeof(uint32_t) * 2) + compressed_size;

        uint32_t aligned_size = (((write_offset + 15) / 16) * 16);
        uint32_t padding = aligned_size - write_offset;
        if (padding)
        {
            target_storage_api->Write(target_storage_api, block_file_handle, write_offset, padding, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0");
            write_offset = aligned_size;
        }

        write_ok = write_ok & target_storage_api->Write(target_storage_api, block_file_handle, write_offset, sizeof(TLongtail_Hash) * chunk_count, &content_index->m_ChunkHashes[first_chunk_index]);
        write_offset += sizeof(TLongtail_Hash) * chunk_count;

        write_ok = write_ok & target_storage_api->Write(target_storage_api, block_file_handle, write_offset, sizeof(uint32_t) * chunk_count, &content_index->m_ChunkLengths[first_chunk_index]);
        write_offset += sizeof(uint32_t) * chunk_count;

        write_ok = write_ok & target_storage_api->Write(target_storage_api, block_file_handle, write_offset, sizeof(uint32_t), &chunk_count);
        write_offset += sizeof(uint32_t);
        target_storage_api->CloseWrite(target_storage_api, block_file_handle);
        write_ok = write_ok & target_storage_api->RenameFile(target_storage_api, tmp_block_path, block_path);
        job->m_Success = write_ok;
    }
    free(block_name);
    block_name = 0;

    free((char*)block_path);
    block_path = 0;

    free((char*)tmp_block_path);
    tmp_block_path = 0;
}

int WriteContent(
    struct StorageAPI* source_storage_api,
    struct StorageAPI* target_storage_api,
    struct CompressionAPI* compression_api,
    struct JobAPI* job_api,
    struct ContentIndex* content_index,
    struct ChunkHashToAssetPart* asset_part_lookup,
    const char* assets_folder,
    const char* content_folder)
{
    LONGTAIL_LOG("WriteContent from `%s` to `%s`, chunks %u, blocks %u\n", assets_folder, content_folder, (uint32_t)*content_index->m_ChunkCount, (uint32_t)*content_index->m_BlockCount)
    uint64_t block_count = *content_index->m_BlockCount;
    if (block_count == 0)
    {
        return 1;
    }

    if (job_api)
    {
        if (!job_api->ReserveJobs(job_api, block_count))
        {
            return 0;
        }
    }

    struct WriteBlockJob** write_block_jobs = (struct WriteBlockJob**)malloc(sizeof(struct WriteBlockJob*) * block_count);
    uint32_t block_start_chunk_index = 0;
    for (uint32_t block_index = 0; block_index < block_count; ++block_index)
    {
        TLongtail_Hash block_hash = content_index->m_BlockHashes[block_index];
        uint32_t chunk_count = 0;
        while(content_index->m_ChunkBlockIndexes[block_start_chunk_index + chunk_count] == block_index)
        {
            ++chunk_count;
        }

        char* block_name = GetBlockName(block_hash);
        char file_name[64];
        sprintf(file_name, "%s.lrb", block_name);
        free(block_name);
        char* block_path = target_storage_api->ConcatPath(target_storage_api, content_folder, file_name);
        if (target_storage_api->IsFile(target_storage_api, block_path))
        {
            write_block_jobs[block_index] = 0;
            free((char*)block_path);
            block_path = 0;
            block_start_chunk_index += chunk_count;
            continue;
        }

        struct WriteBlockJob* job = CreateWriteContentBlockJob();
        write_block_jobs[block_index] = job;
        job->m_SourceStorageAPI = source_storage_api;
        job->m_TargetStorageAPI = target_storage_api;
        job->m_CompressionAPI = compression_api;
        job->m_ContentFolder = content_folder;
        job->m_AssetsFolder = assets_folder;
        job->m_ContentIndex = content_index;
        job->m_AssetPartLookup = asset_part_lookup;
        job->m_FirstChunkIndex = block_start_chunk_index;
        job->m_ChunkCount = chunk_count;
        job->m_Success = 0;

        if (job_api == 0)
        {
            WriteContentBlockJob(job);
        }
        else
        {
            JobAPI_JobFunc func[1] = { WriteContentBlockJob };
            void* ctx[1] = { job };

            job_api->SubmitJobs(job_api, 1, func, ctx);
        }

        block_start_chunk_index += chunk_count;
    }

    if (job_api)
    {
        job_api->WaitForAllJobs(job_api);
    }

    int success = 1;
    while (block_count--)
    {
        struct WriteBlockJob* job = write_block_jobs[block_count];
        if (!job)
        {
            continue;
        }
        if (!job->m_Success)
        {
            success = 0;
        }
        free(job);
    }

    free(write_block_jobs);

    return success;
}

static char* ReadBlockData(
    struct StorageAPI* storage_api,
    struct CompressionAPI* compression_api,
    const char* content_folder,
    TLongtail_Hash block_hash)
{
    char* block_name = GetBlockName(block_hash);
    char file_name[64];
    sprintf(file_name, "%s.lrb", block_name);
    char* block_path = storage_api->ConcatPath(storage_api, content_folder, file_name);
    free(block_name);
    block_name = 0;

    StorageAPI_HOpenFile block_file = storage_api->OpenReadFile(storage_api, block_path);
    if (!block_file)
    {
        LONGTAIL_LOG("Failed to open block `%s`\n", block_path)
        free(block_name);
        block_name = 0;
        free(block_path);
        block_path = 0;
        return 0;
    }
    uint64_t compressed_block_size = storage_api->GetSize(storage_api, block_file);
    char* compressed_block_content = (char*)malloc(compressed_block_size);
    int ok = storage_api->Read(storage_api, block_file, 0, compressed_block_size, compressed_block_content);
    storage_api->CloseRead(storage_api, block_file);
    block_file = 0;
    if (!ok){
        LONGTAIL_LOG("Failed to read block `%s`\n", block_path)
        free(block_name);
        block_name = 0;
        free(block_path);
        block_path = 0;
        free(compressed_block_content);
        compressed_block_content = 0;
        return 0;
    }

    uint32_t uncompressed_size = ((uint32_t*)compressed_block_content)[0];
    uint32_t compressed_size = ((uint32_t*)compressed_block_content)[1];
    char* block_data = (char*)malloc(uncompressed_size);
    CompressionAPI_HDecompressionContext compression_context = compression_api->CreateDecompressionContext(compression_api);
    if (!compression_context)
    {
        LONGTAIL_LOG("Failed to create decompressor for block `%s`\n", block_path)
        free(block_data);
        block_data = 0;
        free(block_name);
        block_name = 0;
        free(block_path);
        block_path = 0;
        free(compressed_block_content);
        compressed_block_content = 0;
        return 0;
    }
    size_t result = compression_api->Decompress(compression_api, compression_context, &compressed_block_content[sizeof(uint32_t) * 2], block_data, compressed_size, uncompressed_size);
    ok = result == uncompressed_size;
    compression_api->DeleteDecompressionContext(compression_api, compression_context);
    free(compressed_block_content);
    compressed_block_content = 0;
    storage_api->CloseRead(storage_api, block_file);
    block_file = 0;

    if (!ok)
    {
        LONGTAIL_LOG("Failed to decompress block `%s`\n", block_path)
        free(block_data);
        block_data = 0;
        free(block_path);
        block_path = 0;
        return 0;
    }
    free(block_path);
    block_path = 0;

    return block_data;
}

struct WriteAssetFromBlocksJob
{
    struct StorageAPI* m_ContentStorageAPI;
    struct StorageAPI* m_VersionStorageAPI;
    struct CompressionAPI* m_CompressionAPI;
    const struct ContentIndex* m_ContentIndex;
    const struct VersionIndex* m_VersionIndex;
    const char* m_ContentFolder;
    const char* m_VersionFolder;
    uint64_t m_AssetIndex;
    struct HashToIndexItem* m_ContentChunkLookup;
    int m_Success;
};

void WriteAssetFromBlocks(void* context)
{
    struct WriteAssetFromBlocksJob* job = (struct WriteAssetFromBlocksJob*)context;
    job->m_Success = 0;
    struct StorageAPI* content_storage_api = job->m_ContentStorageAPI;
    struct StorageAPI* version_storage_api = job->m_VersionStorageAPI;
    struct CompressionAPI* compression_api = job->m_CompressionAPI;
    const char* content_folder = job->m_ContentFolder;
    const char* version_folder = job->m_VersionFolder;
    const uint64_t asset_index = job->m_AssetIndex;
    const struct ContentIndex* content_index = job->m_ContentIndex;
    const struct VersionIndex* version_index = job->m_VersionIndex;
    struct HashToIndexItem* content_chunk_lookup = job->m_ContentChunkLookup;

    const char* asset_path = &version_index->m_NameData[version_index->m_NameOffsets[asset_index]];
	char* full_asset_path = version_storage_api->ConcatPath(version_storage_api, version_folder, asset_path);
	if (!EnsureParentPathExists(version_storage_api, full_asset_path))
	{
		LONGTAIL_LOG("Failed to create parent folder for `%s`\n", full_asset_path)
		free(full_asset_path);
		full_asset_path = 0;
		return;
	}
	if (IsDirPath(full_asset_path))
	{
		if (!SafeCreateDir(version_storage_api, full_asset_path))
		{
			LONGTAIL_LOG("Failed to create folder for `%s`\n", full_asset_path)
			free(full_asset_path);
			full_asset_path = 0;
			return;
		}
        free(full_asset_path);
        full_asset_path = 0;
        job->m_Success = 1;
		return;
	}
    uint32_t* chunk_indexes = &version_index->m_AssetChunkIndexes[version_index->m_AssetChunkIndexStarts[asset_index]];
    uint32_t chunk_count = version_index->m_AssetChunkCounts[asset_index];
    uint32_t chunk_index = 0;
    StorageAPI_HOpenFile asset_file = version_storage_api->OpenWriteFile(version_storage_api, full_asset_path);
    if (!asset_file)
    {
        LONGTAIL_LOG("Unable to create asset `%s`\n", full_asset_path)
        free(full_asset_path);
        full_asset_path = 0;
        return;
    }

    TLongtail_Hash chunk_hash = version_index->m_ChunkHashes[chunk_indexes[chunk_index]];
    uint64_t chunk_content_index = hmget(content_chunk_lookup, chunk_hash);
    TLongtail_Hash block_hash = content_index->m_BlockHashes[content_index->m_ChunkBlockIndexes[chunk_content_index]];
    char* block_data = ReadBlockData(content_storage_api, compression_api, content_folder, block_hash);
    if (!block_data)
    {
        LONGTAIL_LOG("Failed to read block 0x%" PRIx64 " to asset `%s`\n", block_hash, full_asset_path)
        version_storage_api->CloseWrite(version_storage_api, asset_file);
        asset_file = 0;
        free(full_asset_path);
        full_asset_path = 0;
        return;
    }

    uint64_t asset_offset = 0;
    uint32_t chunk_size = content_index->m_ChunkLengths[chunk_content_index];
    uint32_t chunk_offset = content_index->m_ChunkBlockOffsets[chunk_content_index];
    int ok = version_storage_api->Write(version_storage_api, asset_file, asset_offset, chunk_size, &block_data[chunk_offset]);
    if (!ok)
    {
        LONGTAIL_LOG("Failed to write chunk 0x%" PRIx64 " to asset `%s`\n", chunk_hash, full_asset_path)
        free(block_data);
        block_data = 0;
        version_storage_api->CloseWrite(version_storage_api, asset_file);
        asset_file = 0;
        free(full_asset_path);
        full_asset_path = 0;
        return;
    }
    asset_offset += chunk_size;

	++chunk_index;
	while (chunk_index < chunk_count)
    {
        chunk_hash = version_index->m_ChunkHashes[chunk_indexes[chunk_index]];
        chunk_content_index = hmget(content_chunk_lookup, chunk_hash);
        TLongtail_Hash next_block_hash = content_index->m_BlockHashes[content_index->m_ChunkBlockIndexes[chunk_content_index]];
        if (next_block_hash != block_hash)
        {
            free(block_data);
            block_data = 0;
            block_hash = next_block_hash;
            block_data = ReadBlockData(content_storage_api, compression_api, content_folder, block_hash);
            if (!block_data)
            {
                LONGTAIL_LOG("Failed to read block 0x%" PRIx64 " from `%s`\n", block_hash, content_folder)
                version_storage_api->CloseWrite(version_storage_api, asset_file);
                asset_file = 0;
                free(full_asset_path);
                full_asset_path = 0;
                return;
            }
        }
        chunk_size = content_index->m_ChunkLengths[chunk_content_index];
        chunk_offset = content_index->m_ChunkBlockOffsets[chunk_content_index];
        int ok = version_storage_api->Write(version_storage_api, asset_file, asset_offset, chunk_size, &block_data[chunk_offset]);
        if (!ok)
        {
            LONGTAIL_LOG("Failed to write chunk 0x%" PRIx64 " to asset `%s`\n", chunk_hash, full_asset_path)
            free(block_data);
            block_data = 0;
            version_storage_api->CloseWrite(version_storage_api, asset_file);
            asset_file = 0;
            free(full_asset_path);
            full_asset_path = 0;
            return;
        }
        asset_offset += chunk_size;
		++chunk_index;
	}
    free(block_data);
    block_data = 0;
    version_storage_api->CloseWrite(version_storage_api, asset_file);
    asset_file = 0;
    free(full_asset_path);
    full_asset_path = 0;
    job->m_Success = 1;
}

struct WriteAssetsFromBlockJob
{
    struct StorageAPI* m_ContentStorageAPI;
    struct StorageAPI* m_VersionStorageAPI;
    struct CompressionAPI* m_CompressionAPI;
    const struct ContentIndex* m_ContentIndex;
    const struct VersionIndex* m_VersionIndex;
    const char* m_ContentFolder;
    const char* m_VersionFolder;
    uint32_t m_BlockIndex;
    uint64_t* m_AssetIndexes;
    uint32_t m_AssetCount;
    struct HashToIndexItem* m_ContentChunkLookup;
    int m_Success;
};

void WriteAssetsFromBlock(void* context)
{
    struct WriteAssetsFromBlockJob* job = (struct WriteAssetsFromBlockJob*)context;
    job->m_Success = 0;
    struct StorageAPI* content_storage_api = job->m_ContentStorageAPI;
    struct StorageAPI* version_storage_api = job->m_VersionStorageAPI;
    struct CompressionAPI* compression_api = job->m_CompressionAPI;
    const char* content_folder = job->m_ContentFolder;
    const char* version_folder = job->m_VersionFolder;
    const uint32_t block_index = job->m_BlockIndex;
    const struct ContentIndex* content_index = job->m_ContentIndex;
    const struct VersionIndex* version_index = job->m_VersionIndex;
    uint64_t* asset_indexes = job->m_AssetIndexes;
    uint32_t asset_count = job->m_AssetCount;
    struct HashToIndexItem* content_chunk_lookup = job->m_ContentChunkLookup;

    TLongtail_Hash block_hash = content_index->m_BlockHashes[block_index];
    char* block_data = ReadBlockData(content_storage_api, compression_api, content_folder, block_hash);
    if (!block_data)
    {
        LONGTAIL_LOG("Failed to read block 0x%" PRIx64 "\n", block_hash)
    }

    for (uint32_t i = 0; i < asset_count; ++i)
    {
        uint32_t asset_index = asset_indexes[i];
        const char* asset_path = &version_index->m_NameData[version_index->m_NameOffsets[asset_index]];
        char* full_asset_path = version_storage_api->ConcatPath(version_storage_api, version_folder, asset_path);
        int ok = EnsureParentPathExists(version_storage_api, full_asset_path);
        if (!ok)
        {
            free(block_data);
            block_data = 0;
            return;
        }

        StorageAPI_HOpenFile asset_file = version_storage_api->OpenWriteFile(version_storage_api, full_asset_path);
        if (!asset_file)
        {
            free(full_asset_path);
            full_asset_path = 0;
            free(block_data);
            block_data = 0;
            return;
        }

        uint64_t asset_write_offset = 0;
        uint32_t asset_chunk_index_start = version_index->m_AssetChunkIndexStarts[asset_index];
        for (uint32_t asset_chunk_index = 0; asset_chunk_index < version_index->m_AssetChunkCounts[asset_index]; ++asset_chunk_index)
        {
            uint32_t chunk_index = version_index->m_AssetChunkIndexes[asset_chunk_index_start + asset_chunk_index];
            TLongtail_Hash chunk_hash = version_index->m_ChunkHashes[chunk_index];
            uint64_t content_chunk_index = hmget(content_chunk_lookup, chunk_hash);
            uint32_t chunk_block_offset = content_index->m_ChunkBlockOffsets[content_chunk_index];
            uint32_t chunk_size = content_index->m_ChunkLengths[content_chunk_index];
            ok = version_storage_api->Write(version_storage_api, asset_file, asset_write_offset, chunk_size, &block_data[chunk_block_offset]);
            if (!ok)
            {
                content_storage_api->CloseWrite(version_storage_api, asset_file);
                asset_file = 0;
                free(full_asset_path);
                full_asset_path = 0;
                free(block_data);
                block_data = 0;
                return;
            }
			asset_write_offset += chunk_size;
        }

        version_storage_api->CloseWrite(version_storage_api, asset_file);
        asset_file = 0;

        free(full_asset_path);
        full_asset_path = 0;
    }

    free(block_data);
    block_data = 0;
    job->m_Success = 1;
}

struct BlockJobCompareContext
{
    const struct VersionIndex* m_VersionIndex;
    struct HashToIndexItem* m_ChunkHashToBlockIndex;
};

static int BlockJobCompare(void* context, const void* a_ptr, const void* b_ptr)
{
    struct BlockJobCompareContext* c = (struct BlockJobCompareContext*)context;
    const struct VersionIndex* version_index = c->m_VersionIndex;
    struct HashToIndexItem* chunk_hash_to_block_index = c->m_ChunkHashToBlockIndex;

    uint64_t a = *(uint64_t*)a_ptr;
    uint64_t b = *(uint64_t*)b_ptr;
    TLongtail_Hash a_first_chunk_hash = version_index->m_ChunkHashes[version_index->m_AssetChunkIndexes[version_index->m_AssetChunkIndexStarts[a]]];
    TLongtail_Hash b_first_chunk_hash = version_index->m_ChunkHashes[version_index->m_AssetChunkIndexes[version_index->m_AssetChunkIndexStarts[b]]];
    if (a_first_chunk_hash == b_first_chunk_hash)
    {
        return 0;
    }
    uint64_t a_block_index = hmget(c->m_ChunkHashToBlockIndex, a_first_chunk_hash);
	uint64_t b_block_index = hmget(c->m_ChunkHashToBlockIndex, b_first_chunk_hash);
    if (a_block_index == b_block_index)
    {
        return 0;
    }
    else if (a_block_index < b_block_index)
    {
        return -1;
    }
    else if (a_block_index > b_block_index)
    {
        return 1;
    }
    return 0;
}

int WriteVersion(
    struct StorageAPI* content_storage_api,
    struct StorageAPI* version_storage_api,
    struct CompressionAPI* compression_api,
    struct JobAPI* job_api,
    const struct ContentIndex* content_index,
    const struct VersionIndex* version_index,
    const char* content_path,
    const char* version_path)
{
    LONGTAIL_LOG("WriteVersion from `%s` to `%s`, assets %u\n", content_path, version_path, (uint32_t)*version_index->m_AssetCount);
    struct HashToIndexItem* block_hash_to_block_index = 0;
    for (uint64_t i = 0; i < *content_index->m_BlockCount; ++i)
    {
        TLongtail_Hash block_hash = content_index->m_BlockHashes[i];
        hmput(block_hash_to_block_index, block_hash, i);
    }

    struct HashToIndexItem* chunk_hash_to_content_chunk_index = 0;
    for (uint64_t i = 0; i < *content_index->m_ChunkCount; ++i)
    {
        TLongtail_Hash chunk_hash = content_index->m_ChunkHashes[i];
        hmput(chunk_hash_to_content_chunk_index, chunk_hash, i);
    }

    struct HashToIndexItem* chunk_hash_to_block_index = 0;
    for (uint64_t i = 0; i < *content_index->m_ChunkCount; ++i)
    {
        TLongtail_Hash chunk_hash = content_index->m_ChunkHashes[i];
        intptr_t content_chunk_index = hmgeti(chunk_hash_to_content_chunk_index, chunk_hash);
        if (-1 == content_chunk_index)
        {
            hmfree(chunk_hash_to_content_chunk_index);
            hmfree(chunk_hash_to_block_index);
            hmfree(block_hash_to_block_index);
            return 0;
        }
        hmput(chunk_hash_to_block_index, chunk_hash, content_index->m_ChunkBlockIndexes[content_chunk_index]);
    }

    uint64_t asset_count = *version_index->m_AssetCount;
    uint64_t block_job_count = 0;
    uint64_t* block_job_asset_indexes = (uint64_t*)malloc(sizeof(uint64_t) * (*content_index->m_BlockCount));
    uint64_t asset_job_count = 0;
    uint64_t* asset_index_jobs = (uint64_t*)malloc(sizeof(uint64_t) * (*version_index->m_AssetCount));
    uint64_t asset_chunk_offset = 0;

    for (uint64_t i = 0; i < asset_count; ++i)
    {
        const char* path = &version_index->m_NameData[version_index->m_NameOffsets[i]];
        uint32_t chunk_count = version_index->m_AssetChunkCounts[i];
        uint32_t asset_chunk_offset = version_index->m_AssetChunkIndexStarts[i];
        if (chunk_count == 0)
        {
            asset_index_jobs[asset_job_count] = i;
            ++asset_job_count;
            continue;
        }
        uint32_t chunk_index = version_index->m_AssetChunkIndexes[asset_chunk_offset];
        TLongtail_Hash chunk_hash = version_index->m_ChunkHashes[chunk_index];
		uint64_t content_block_index = hmget(chunk_hash_to_block_index, chunk_hash);
        int is_block_job = 1;
        for (uint32_t c = 1; c < chunk_count; ++c)
        {
            uint32_t next_chunk_index = version_index->m_AssetChunkIndexes[asset_chunk_offset + c];
            TLongtail_Hash next_chunk_hash = version_index->m_ChunkHashes[next_chunk_index];
			uint64_t next_content_block_index = hmget(chunk_hash_to_block_index, next_chunk_hash); // TODO: Validate existance!
            if (content_block_index != next_content_block_index)
            {
                is_block_job = 0;
                // We don't break here since we want to validate that all the chunks are in the content index
            }
        }

        if (is_block_job)
        {
            block_job_asset_indexes[block_job_count] = i;
            ++block_job_count;
        }
        else
        {
            asset_index_jobs[asset_job_count] = i;
            ++asset_job_count;
        }
    }

    if (job_api)
    {
        if (!job_api->ReserveJobs(job_api, block_job_count + asset_job_count))
        {
            return 0;
        }
    }

    struct BlockJobCompareContext block_job_compare_context = { version_index, chunk_hash_to_block_index };
    qsort_s(block_job_asset_indexes, block_job_count, sizeof(uint64_t), BlockJobCompare, &block_job_compare_context);

    struct WriteAssetsFromBlockJob* block_jobs = (struct WriteAssetsFromBlockJob*)malloc(sizeof(struct WriteAssetsFromBlockJob) * block_job_count);
    uint32_t b = 0;
    while (b < block_job_count)
    {
        uint32_t asset_index = block_job_asset_indexes[b];
        TLongtail_Hash first_chunk_hash = version_index->m_ChunkHashes[version_index->m_AssetChunkIndexes[version_index->m_AssetChunkIndexStarts[asset_index]]];
        uint64_t block_index = hmget(chunk_hash_to_block_index, first_chunk_hash);
        struct WriteAssetsFromBlockJob* job = &block_jobs[b];
        job->m_ContentStorageAPI = content_storage_api;
        job->m_VersionStorageAPI = version_storage_api;
        job->m_CompressionAPI = compression_api;
        job->m_ContentIndex = content_index;
        job->m_VersionIndex = version_index;
        job->m_ContentFolder = content_path;
        job->m_VersionFolder = version_path;
        job->m_BlockIndex = (uint64_t)block_index;
        job->m_ContentChunkLookup = chunk_hash_to_content_chunk_index;
        job->m_AssetIndexes = &block_job_asset_indexes[b];

        job->m_AssetCount = 1;
        ++b;
        while (b < block_job_count)
        {
            uint32_t next_asset_index = block_job_asset_indexes[b];
            TLongtail_Hash next_first_chunk_hash = version_index->m_ChunkHashes[version_index->m_AssetChunkIndexes[version_index->m_AssetChunkIndexStarts[next_asset_index]]];
            intptr_t next_block_index = hmget(chunk_hash_to_block_index, next_first_chunk_hash);
            if (block_index != next_block_index)
            {
                break;
            }

            ++job->m_AssetCount;
            ++b;
        }
        if (job_api == 0)
        {
            WriteAssetsFromBlock(job);
        }
        else
        {
            JobAPI_JobFunc func[1] = { WriteAssetsFromBlock };
            void* ctx[1] = { job };

            job_api->SubmitJobs(job_api, 1, func, ctx);
        }
    }

    struct WriteAssetFromBlocksJob* asset_jobs = (struct WriteAssetFromBlocksJob*)malloc(sizeof(struct WriteAssetFromBlocksJob) * asset_job_count);
    for (uint32_t a = 0; a < asset_job_count; ++a)
    {
        struct WriteAssetFromBlocksJob* job = &asset_jobs[a];
        job->m_ContentStorageAPI = content_storage_api;
        job->m_VersionStorageAPI = version_storage_api;
        job->m_CompressionAPI = compression_api;
        job->m_ContentIndex = content_index;
        job->m_VersionIndex = version_index;
        job->m_ContentFolder = content_path;
        job->m_VersionFolder = version_path;
		job->m_ContentChunkLookup = chunk_hash_to_content_chunk_index;
		job->m_AssetIndex = asset_index_jobs[a];
        if (job_api == 0)
        {
            WriteAssetFromBlocks(job);
        }
        else
        {
            JobAPI_JobFunc func[1] = { WriteAssetFromBlocks };
            void* ctx[1] = { job };

            job_api->SubmitJobs(job_api, 1, func, ctx);
        }
    }

    if (job_api)
    {
        job_api->WaitForAllJobs(job_api);
    }

    int success = 1;
    for (uint32_t b = 0; b < block_job_count; ++b)
    {
        struct WriteAssetsFromBlockJob* job = &block_jobs[b];
        if (!job)
        {
            success = 0;
        }
    }
    for (uint32_t a = 0; a < asset_job_count; ++a)
    {
        struct WriteAssetFromBlocksJob* job = &asset_jobs[a];
        if (!job)
        {
            success = 0;
        }
    }

    free(asset_jobs);
    asset_jobs = 0;
    free(block_jobs);
    block_jobs = 0;

    free(asset_index_jobs);
    asset_index_jobs = 0;

    free(block_job_asset_indexes);
    block_job_asset_indexes = 0;

    hmfree(chunk_hash_to_content_chunk_index);
    chunk_hash_to_content_chunk_index = 0;

    hmfree(block_hash_to_block_index);
    block_hash_to_block_index = 0;

    return success;
}

struct BlockIndex* ReadBlock(
    struct StorageAPI* storage_api,
    struct HashAPI* hash_api,
    const char* full_block_path)
{
    StorageAPI_HOpenFile f = storage_api->OpenReadFile(storage_api, full_block_path);
    if (!f)
    {
        return 0;
    }
    uint64_t s = storage_api->GetSize(storage_api, f);
    if (s < (sizeof(uint32_t)))
    {
        storage_api->CloseRead(storage_api, f);
        return 0;
    }
    uint32_t chunk_count = 0;
    if (!storage_api->Read(storage_api, f, s - sizeof(uint32_t), sizeof(uint32_t), &chunk_count))
    {
        storage_api->CloseRead(storage_api, f);
        return 0;
    }
    size_t block_index_data_size = GetBlockIndexDataSize(chunk_count);
    if (s < block_index_data_size)
    {
        storage_api->CloseRead(storage_api, f);
        return 0;
    }

    struct BlockIndex* block_index = InitBlockIndex(malloc(GetBlockIndexSize(chunk_count)), chunk_count);

    int ok = storage_api->Read(storage_api, f, s - block_index_data_size, block_index_data_size, &block_index[1]);
    storage_api->CloseRead(storage_api, f);
    if (!ok)
    {
        free(block_index);
        return 0;
    }
    HashAPI_HContext hash_context = hash_api->BeginContext(hash_api);
    hash_api->Hash(hash_api, hash_context, (uint32_t)(GetBlockIndexDataSize(chunk_count)), (void*)&block_index[1]);
    TLongtail_Hash block_hash = hash_api->EndContext(hash_api, hash_context);
    block_index->m_BlockHash = block_hash;

    return block_index;
}

struct ReadContentContext {
    struct StorageAPI* m_StorageAPI;
    uint32_t m_ReservedPathCount;
    uint32_t m_ReservedPathSize;
    uint32_t m_RootPathLength;
    struct Paths* m_Paths;
    uint64_t m_ChunkCount;
};

void ReadContentAddPath(void* context, const char* root_path, const char* file_name)
{
    struct ReadContentContext* paths_context = (struct ReadContentContext*)context;
    struct StorageAPI* storage_api = paths_context->m_StorageAPI;

    char* full_path = storage_api->ConcatPath(storage_api, root_path, file_name);
    if (storage_api->IsDir(storage_api, full_path))
    {
        free(full_path);
        return;
    }

    struct Paths* paths = paths_context->m_Paths;
    const uint32_t root_path_length = paths_context->m_RootPathLength;
    const char* s = &full_path[root_path_length];
    if (*s == '/')
    {
        ++s;
    }

    paths_context->m_Paths = AppendPath(paths_context->m_Paths, s, &paths_context->m_ReservedPathCount, &paths_context->m_ReservedPathSize, 512, 128);

    free(full_path);
    full_path = 0;
};

struct ContentIndex* ReadContent(
    struct StorageAPI* storage_api,
    struct HashAPI* hash_api,
    struct JobAPI* job_api,
    const char* content_path)
{
    LONGTAIL_LOG("ReadContent from `%s`\n", content_path)

    const uint32_t default_path_count = 512;
    const uint32_t default_path_data_size = default_path_count * 128;

    struct Paths* paths = CreatePaths(default_path_count, default_path_data_size);
    struct ReadContentContext context = {storage_api, default_path_count, default_path_data_size, (uint32_t)(strlen(content_path)), paths, 0};
    if(!RecurseTree(storage_api, content_path, ReadContentAddPath, &context))
    {
        free(context.m_Paths);
        return 0;
    }
    paths = context.m_Paths;

    TLongtail_Hash* block_hashes = 0;
    block_hashes = arrsetcap(block_hashes, *paths->m_PathCount);
    TLongtail_Hash* chunk_hashes = 0;
    chunk_hashes = arrsetcap(chunk_hashes, *paths->m_PathCount);
    uint32_t* chunk_lengths = 0;
    chunk_lengths = arrsetcap(chunk_lengths, *paths->m_PathCount);
    uint64_t* chunk_block_indexes = 0;
    chunk_block_indexes = arrsetcap(chunk_block_indexes, *paths->m_PathCount * 16);
    uint32_t* chunk_block_offsets = 0;
    chunk_block_offsets = arrsetcap(chunk_block_offsets, *paths->m_PathCount * 16);

    // TODO: use job api!
    LONGTAIL_LOG("Scanning %u files from `%s`\n", *paths->m_PathCount, content_path);
    for (uint32_t path_index = 0; path_index < *paths->m_PathCount; ++path_index)
    {
        const char* block_path = &paths->m_Data[paths->m_Offsets[path_index]];
        char* full_block_path = storage_api->ConcatPath(storage_api, content_path, block_path);

        struct BlockIndex* block_index = ReadBlock(
            storage_api,
            hash_api,
            full_block_path);

        free(full_block_path);
        full_block_path = 0;

        if (!block_index)
        {
            // Not a valid block
            continue;
        }

        uint64_t block_count = (uint64_t)arrlen(block_hashes);
        uint64_t chunk_count = (uint64_t)arrlen(chunk_hashes);
        uint32_t block_chunk_count = *block_index->m_ChunkCount;

        arrsetlen(chunk_hashes, chunk_count + block_chunk_count);
        arrsetlen(chunk_lengths, chunk_count + block_chunk_count);
        arrsetlen(chunk_block_indexes, chunk_count + block_chunk_count);
        arrsetlen(chunk_block_offsets, chunk_count + block_chunk_count);

        uint32_t block_offset = 0;
        for (uint32_t a = 0; a < block_chunk_count; ++a)
        {
            chunk_hashes[chunk_count + a] = block_index->m_ChunkHashes[a];
            chunk_lengths[chunk_count + a] = block_index->m_ChunkSizes[a];
            chunk_block_indexes[chunk_count + a] = block_count;
            chunk_block_offsets[chunk_count + a] = block_offset;
            block_offset += block_index->m_ChunkSizes[a];
        }
        arrpush(block_hashes, block_index->m_BlockHash);

        free(block_index);
        block_index = 0;
    }

    free(paths);
    paths = 0;

    uint64_t block_count = (uint64_t)arrlen(block_hashes);
    uint64_t chunk_count = (uint64_t)arrlen(chunk_hashes);

    LONGTAIL_LOG("Found %" PRIu64 " chunks in %" PRIu64 " blocks from `%s`\n", chunk_count, block_count, content_path);

    size_t content_index_data_size = GetContentIndexDataSize(block_count, chunk_count);
    struct ContentIndex* content_index = (struct ContentIndex*)malloc(sizeof(struct ContentIndex) + content_index_data_size);
    content_index->m_BlockCount = (uint64_t*) & ((char*)content_index)[sizeof(struct ContentIndex)];
    content_index->m_ChunkCount = (uint64_t*) & ((char*)content_index)[sizeof(struct ContentIndex) + sizeof(uint64_t)];
    *content_index->m_BlockCount = block_count;
    *content_index->m_ChunkCount = chunk_count;
    InitContentIndex(content_index);

    memmove(content_index->m_BlockHashes, block_hashes, sizeof(TLongtail_Hash) * block_count);
    memmove(content_index->m_ChunkHashes, chunk_hashes, sizeof(TLongtail_Hash) * chunk_count);
    memmove(content_index->m_ChunkBlockIndexes, chunk_block_indexes, sizeof(uint64_t) * chunk_count);
    memmove(content_index->m_ChunkBlockOffsets, chunk_block_offsets, sizeof(uint32_t) * chunk_count);
    memmove(content_index->m_ChunkLengths, chunk_lengths, sizeof(uint32_t) * chunk_count);

    arrfree(block_hashes);
    block_hashes = 0;
    arrfree(chunk_hashes);
    chunk_hashes = 0;
    arrfree(chunk_block_indexes);
    chunk_block_indexes = 0;
    arrfree(chunk_block_offsets);
    chunk_block_offsets = 0;
    arrfree(chunk_lengths);
    chunk_lengths = 0;

    return content_index;
}

int CompareHash(const void* a_ptr, const void* b_ptr) 
{
    TLongtail_Hash a = *((TLongtail_Hash*)a_ptr);
    TLongtail_Hash b = *((TLongtail_Hash*)b_ptr);
    if (a > b) return  1;
    if (a < b) return -1;
    return 0;
}

uint32_t MakeUnique(TLongtail_Hash* hashes, uint32_t count)
{
    uint32_t w = 0;
    uint32_t r = 0;
    while (r < count)
    {
        hashes[w] = hashes[r];
        ++r;
        while (r < count && hashes[r - 1] == hashes[r])
        {
            ++r;
        }
        ++w;
    }
    return w;
}

void DiffHashes(
    const TLongtail_Hash* reference_hashes,
    uint32_t reference_hash_count,
    const TLongtail_Hash* new_hashes,
    uint32_t new_hash_count,
    uint32_t* added_hash_count,
    TLongtail_Hash* added_hashes,
    uint32_t* removed_hash_count,
    TLongtail_Hash* removed_hashes)
{
    TLongtail_Hash* refs = (TLongtail_Hash*)malloc(sizeof(TLongtail_Hash) * reference_hash_count);
    TLongtail_Hash* news = (TLongtail_Hash*)malloc(sizeof(TLongtail_Hash) * new_hash_count);
    memmove(refs, reference_hashes, sizeof(TLongtail_Hash) * reference_hash_count);
    memmove(news, new_hashes, sizeof(TLongtail_Hash) * new_hash_count);

    qsort(&refs[0], reference_hash_count, sizeof(TLongtail_Hash), CompareHash);
    reference_hash_count = MakeUnique(&refs[0], reference_hash_count);

    qsort(&news[0], new_hash_count, sizeof(TLongtail_Hash), CompareHash);
    new_hash_count = MakeUnique(&news[0], new_hash_count);

    uint32_t removed = 0;
    uint32_t added = 0;
    uint32_t ni = 0;
    uint32_t ri = 0;
    while (ri < reference_hash_count && ni < new_hash_count)
    {
        if (refs[ri] == news[ni])
        {
            ++ri;
            ++ni;
            continue;
        }
        else if (refs[ri] < news[ni])
        {
            if (removed_hashes)
            {
                removed_hashes[removed] = refs[ri];
            }
            ++removed;
            ++ri;
        }
        else if (refs[ri] > news[ni])
        {
            added_hashes[added++] = news[ni++];
        }
    }
    while (ni < new_hash_count)
    {
        added_hashes[added++] = news[ni++];
    }
    *added_hash_count = added;
    while (ri < reference_hash_count)
    {
        if (removed_hashes)
        {
            removed_hashes[removed] = refs[ri];
        }
        ++removed;
        ++ri;
    }
    if (removed_hash_count)
    {
        *removed_hash_count = removed;
    }

    free(news);
    news = 0;
    free(refs);
    refs = 0;
}

struct ContentIndex* CreateMissingContent(
    struct HashAPI* hash_api,
    const struct ContentIndex* content_index,
    const struct VersionIndex* version,
    uint32_t max_block_size,
    uint32_t max_chunks_per_block)
{
    LONGTAIL_LOG("CreateMissingContent\n")
    uint64_t chunk_count = *version->m_ChunkCount;
    TLongtail_Hash* added_hashes = (TLongtail_Hash*)malloc(sizeof(TLongtail_Hash) * chunk_count);

    uint32_t added_hash_count = 0;
    DiffHashes(content_index->m_ChunkHashes, *content_index->m_ChunkCount, version->m_ChunkHashes, chunk_count, &added_hash_count, added_hashes, 0, 0);

    if (added_hash_count == 0)
    {
        free(added_hashes);
        added_hashes = 0;
        struct ContentIndex* diff_content_index = CreateContentIndex(
            hash_api,
            0,
            0,
            0,
            max_block_size,
            max_chunks_per_block);
        return diff_content_index;
    }

    uint32_t* diff_chunk_sizes = (uint32_t*)malloc(sizeof(uint32_t) * added_hash_count);

    struct HashToIndexItem* chunk_index_lookup = 0;
    for (uint64_t i = 0; i < chunk_count; ++i)
    {
        hmput(chunk_index_lookup, version->m_ChunkHashes[i], i);
    }

    for (uint32_t j = 0; j < added_hash_count; ++j)
    {
        ptrdiff_t lookup_index = hmgeti(chunk_index_lookup, added_hashes[j]);
        if (lookup_index == -1)
        {
            hmfree(chunk_index_lookup);
            free(added_hashes);
            return 0;
        }

        uint64_t chunk_index = chunk_index_lookup[lookup_index].value;
        diff_chunk_sizes[j] = version->m_ChunkSizes[chunk_index];
    }
    hmfree(chunk_index_lookup);
    chunk_index_lookup = 0;

    struct ContentIndex* diff_content_index = CreateContentIndex(
        hash_api,
        added_hash_count,
        added_hashes,
        diff_chunk_sizes,
        max_block_size,
        max_chunks_per_block);

    free(added_hashes);
    added_hashes = 0;

    return diff_content_index;
}

// TODO: This could be more efficient - if a block exists in both local_content_index and remote_content_index it will
// be present twice in the resulting content index. This is fine but a waste.
struct ContentIndex* MergeContentIndex(
    struct ContentIndex* local_content_index,
    struct ContentIndex* remote_content_index)
{
    uint64_t local_block_count = *local_content_index->m_BlockCount;
    uint64_t remote_block_count = *remote_content_index->m_BlockCount;
    uint64_t local_chunk_count = *local_content_index->m_ChunkCount;
    uint64_t remote_chunk_count = *remote_content_index->m_ChunkCount;
    uint64_t block_count = local_block_count + remote_block_count;
    uint64_t chunk_count = local_chunk_count + remote_chunk_count;
    size_t content_index_size = GetContentIndexSize(block_count, chunk_count);
    struct ContentIndex* content_index = (struct ContentIndex*)malloc(content_index_size);

    content_index->m_BlockCount = (uint64_t*)&((char*)content_index)[sizeof(struct ContentIndex)];
    content_index->m_ChunkCount = (uint64_t*)&((char*)content_index)[sizeof(struct ContentIndex) + sizeof(uint64_t)];
    *content_index->m_BlockCount = block_count;
    *content_index->m_ChunkCount = chunk_count;
    InitContentIndex(content_index);

    for (uint64_t b = 0; b < local_block_count; ++b)
    {
        content_index->m_BlockHashes[b] = local_content_index->m_BlockHashes[b];
    }
    for (uint64_t b = 0; b < remote_block_count; ++b)
    {
        content_index->m_BlockHashes[local_block_count + b] = remote_content_index->m_BlockHashes[b];
    }
    for (uint64_t a = 0; a < local_chunk_count; ++a)
    {
        content_index->m_ChunkHashes[a] = local_content_index->m_ChunkHashes[a];
        content_index->m_ChunkBlockIndexes[a] = local_content_index->m_ChunkBlockIndexes[a];
        content_index->m_ChunkBlockOffsets[a] = local_content_index->m_ChunkBlockOffsets[a];
        content_index->m_ChunkLengths[a] = local_content_index->m_ChunkLengths[a];
    }
    for (uint64_t a = 0; a < remote_chunk_count; ++a)
    {
        content_index->m_ChunkHashes[local_chunk_count + a] = remote_content_index->m_ChunkHashes[a];
        content_index->m_ChunkBlockIndexes[local_chunk_count + a] = local_block_count + remote_content_index->m_ChunkBlockIndexes[a];
        content_index->m_ChunkBlockOffsets[local_chunk_count + a] = remote_content_index->m_ChunkBlockOffsets[a];
        content_index->m_ChunkLengths[local_chunk_count + a] = remote_content_index->m_ChunkLengths[a];
    }
    return content_index;
}
