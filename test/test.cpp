#include "../src/longtail.h"

#include "../third-party/jctest/src/jc_test.h"

#include "../lib/longtail_lib.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define TEST_LOG(fmt, ...) \
    fprintf(stderr, "--- ");fprintf(stderr, fmt, __VA_ARGS__);

int CreateParentPath(struct StorageAPI* storage_api, const char* path)
{
    char* dir_path = Longtail_Strdup(path);
    char* last_path_delimiter = (char*)strrchr(dir_path, '/');
    if (last_path_delimiter == 0)
    {
        Longtail_Free(dir_path);
        return 1;
    }
    *last_path_delimiter = '\0';
    if (storage_api->IsDir(storage_api, dir_path))
    {
        Longtail_Free(dir_path);
        return 1;
    }
    else
    {
        if (!CreateParentPath(storage_api, dir_path))
        {
            TEST_LOG("CreateParentPath failed: `%s`\n", dir_path)
            Longtail_Free(dir_path);
            return 0;
        }
        if (0 == storage_api->CreateDir(storage_api, dir_path))
        {
            Longtail_Free(dir_path);
            return 1;
        }
    }
    TEST_LOG("CreateParentPath failed: `%s`\n", dir_path)
    Longtail_Free(dir_path);
    return 0;
}




int MakePath(StorageAPI* storage_api, const char* path)
{
    char* dir_path = Longtail_Strdup(path);
    char* last_path_delimiter = (char*)strrchr(dir_path, '/');
    if (last_path_delimiter == 0)
    {
        Longtail_Free(dir_path);
        dir_path = 0;
        return 1;
    }
    *last_path_delimiter = '\0';
    if (storage_api->IsDir(storage_api, dir_path))
    {
        Longtail_Free(dir_path);
        return 1;
    }
    else
    {
        if (!MakePath(storage_api, dir_path))
        {
            TEST_LOG("MakePath failed: `%s`\n", dir_path)
            Longtail_Free(dir_path);
            return 0;
        }
        if (0 == storage_api->CreateDir(storage_api, dir_path))
        {
            Longtail_Free(dir_path);
            return 1;
        }
        if (storage_api->IsDir(storage_api, dir_path))
        {
            Longtail_Free(dir_path);
            return 1;
        }
        return 0;
    }
}

static int CreateFakeContent(StorageAPI* storage_api, const char* parent_path, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        char path[128];
        sprintf(path, "%s%s%u", parent_path ? parent_path : "", parent_path && parent_path[0] ? "/" : "", i);
        if (0 == MakePath(storage_api, path))
        {
            return 0;
        }
        StorageAPI_HOpenFile content_file;
        if (storage_api->OpenWriteFile(storage_api, path, 0, &content_file))
        {
            return 0;
        }
        uint64_t content_size = 64000 + 1 + i;
        char* data = (char*)Longtail_Alloc(sizeof(char) * content_size);
        memset(data, i, content_size);
        int err = storage_api->Write(storage_api, content_file, 0, content_size, data);
        Longtail_Free(data);
        if (err)
        {
            return 0;
        }
        storage_api->CloseFile(storage_api, content_file);
    }
    return 1;
}

TEST(Longtail, LongtailMalloc)
{
    void* p = Longtail_Alloc(77);
    ASSERT_NE((void*)0, p);
    Longtail_Free(p);
}

TEST(Longtail, VersionIndex)
{
    const char* asset_paths[5] = {
        "fifth_",
        "fourth",
        "third_",
        "second",
        "first_"
    };

    const TLongtail_Hash asset_path_hashes[5] = {50, 40, 30, 20, 10};
    const TLongtail_Hash asset_content_hashes[5] = { 5, 4, 3, 2, 1};
    const uint64_t asset_sizes[5] = {64003, 64003, 64002, 64001, 64001};
    const uint32_t chunk_sizes[5] = {64003, 64003, 64002, 64001, 64001};
    const uint32_t asset_chunk_counts[5] = {1, 1, 1, 1, 1};
    const uint32_t asset_chunk_start_index[5] = {0, 1, 2, 3, 4};
    const uint32_t asset_compression_types[5] = {0, 0, 0, 0, 0};

    Paths* paths = MakePaths(5, asset_paths);
    size_t version_index_size = GetVersionIndexSize(5, 5, 5, paths->m_DataSize);
    void* version_index_mem = Longtail_Alloc(version_index_size);

    VersionIndex* version_index = BuildVersionIndex(
        version_index_mem,
        version_index_size,
        paths,
        asset_path_hashes,
        asset_content_hashes,
        asset_sizes,
        asset_chunk_start_index,
        asset_chunk_counts,
        *paths->m_PathCount,
        asset_chunk_start_index,
        *paths->m_PathCount,
        chunk_sizes,
        asset_content_hashes,
        asset_compression_types);

    Longtail_Free(version_index);
    Longtail_Free(paths);
}

TEST(Longtail, ContentIndex)
{
    const char* assets_path = "";
    const uint64_t asset_count = 5;
    const TLongtail_Hash asset_content_hashes[5] = { 5, 4, 3, 2, 1};
    const TLongtail_Hash asset_path_hashes[5] = {50, 40, 30, 20, 10};
    const uint32_t asset_sizes[5] = { 43593, 43593, 43592, 43591, 43591 };
    const uint32_t asset_compression_types[5] = {0, 0, 0, 0, 0};
    const uint32_t asset_name_offsets[5] = { 7 * 0, 7 * 1, 7 * 2, 7 * 3, 7 * 4};
    const char* asset_name_data = { "fifth_\0" "fourth\0" "third_\0" "second\0" "first_\0" };

    static const uint32_t MAX_BLOCK_SIZE = 65536 * 2;
    static const uint32_t MAX_CHUNKS_PER_BLOCK = 4096;
    HashAPI* hash_api = CreateMeowHashAPI();
    ASSERT_NE((HashAPI*)0, hash_api);
    HashAPI_HContext c;
    int err = hash_api->BeginContext(hash_api, &c);
    ASSERT_EQ(0, err);
    ASSERT_NE((HashAPI_HContext)0, c);
    hash_api->EndContext(hash_api, c);
    ContentIndex* content_index = CreateContentIndex(
        hash_api,
        asset_count,
        asset_content_hashes,
        asset_sizes,
        asset_compression_types,
        MAX_BLOCK_SIZE,
        MAX_CHUNKS_PER_BLOCK);

    ASSERT_EQ(2u, *content_index->m_BlockCount);
    ASSERT_EQ(5u, *content_index->m_ChunkCount);
    for (uint32_t i = 0; i < *content_index->m_ChunkCount; ++i)
    {
        ASSERT_EQ(asset_content_hashes[i], content_index->m_ChunkHashes[i]);
        ASSERT_EQ(asset_sizes[i], content_index->m_ChunkLengths[i]);
    }
    ASSERT_EQ(0u, content_index->m_ChunkBlockIndexes[0]);
    ASSERT_EQ(0u, content_index->m_ChunkBlockIndexes[1]);
    ASSERT_EQ(0u, content_index->m_ChunkBlockIndexes[2]);
    ASSERT_EQ(1u, content_index->m_ChunkBlockIndexes[3]);
    ASSERT_EQ(1u, content_index->m_ChunkBlockIndexes[4]);

    ASSERT_EQ(0u, content_index->m_ChunkBlockOffsets[0]);
    ASSERT_EQ(43593, content_index->m_ChunkBlockOffsets[1]);
    ASSERT_EQ(43593 * 2, content_index->m_ChunkBlockOffsets[2]);
    ASSERT_EQ(0u, content_index->m_ChunkBlockOffsets[3]);
    ASSERT_EQ(43591, content_index->m_ChunkBlockOffsets[4]);

    Longtail_Free(content_index);

    DestroyHashAPI(hash_api);
}

static uint32_t* GetCompressionTypes(StorageAPI* , const FileInfos* file_infos)
{
    uint32_t count = *file_infos->m_Paths.m_PathCount;
    uint32_t* result = (uint32_t*)Longtail_Alloc(sizeof(uint32_t) * count);
    for (uint32_t i = 0; i < count; ++i)
    {
        result[i] = LIZARD_DEFAULT_COMPRESSION_TYPE;
    }
    return result;
}

TEST(Longtail, ContentIndexSerialization)
{
    StorageAPI* local_storage = CreateInMemStorageAPI();
    HashAPI* hash_api = CreateMeowHashAPI();
    JobAPI* job_api = CreateBikeshedJobAPI(0);

    ASSERT_EQ(1, CreateFakeContent(local_storage, "source/version1/two_items", 2));
    ASSERT_EQ(1, CreateFakeContent(local_storage, "source/version1/five_items", 5));
    FileInfos* version1_paths;
    ASSERT_EQ(0, GetFilesRecursively(local_storage, "source/version1", &version1_paths));
    ASSERT_NE((FileInfos*)0, version1_paths);
    uint32_t* compression_types = GetCompressionTypes(local_storage, version1_paths);
    ASSERT_NE((uint32_t*)0, compression_types);
    VersionIndex* vindex = CreateVersionIndex(
        local_storage,
        hash_api,
        job_api,
        0,
        0,
        "source/version1",
        &version1_paths->m_Paths,
        version1_paths->m_FileSizes,
        compression_types,
        16384);
    ASSERT_NE((VersionIndex*)0, vindex);
    Longtail_Free(compression_types);
    Longtail_Free(version1_paths);

    static const uint32_t MAX_BLOCK_SIZE = 65536 * 2;
    static const uint32_t MAX_CHUNKS_PER_BLOCK = 4096;
    ContentIndex* cindex = CreateContentIndex(
        hash_api,
        *vindex->m_ChunkCount,
        vindex->m_ChunkHashes,
        vindex->m_ChunkSizes,
        vindex->m_ChunkCompressionTypes,
        MAX_BLOCK_SIZE,
        MAX_CHUNKS_PER_BLOCK);
    ASSERT_NE((ContentIndex*)0, cindex);

    Longtail_Free(vindex);
    vindex = 0;

    ASSERT_NE(0, WriteContentIndex(local_storage, cindex, "cindex.lci"));

    ContentIndex* cindex2 = ReadContentIndex(local_storage, "cindex.lci");
    ASSERT_NE((ContentIndex*)0, cindex2);

    ASSERT_EQ(*cindex->m_BlockCount, *cindex2->m_BlockCount);
    for (uint64_t i = 0; i < *cindex->m_BlockCount; ++i)
    {
        ASSERT_EQ(cindex->m_BlockHashes[i], cindex2->m_BlockHashes[i]);
    }
    ASSERT_EQ(*cindex->m_ChunkCount, *cindex2->m_ChunkCount);
    for (uint64_t i = 0; i < *cindex->m_ChunkCount; ++i)
    {
        ASSERT_EQ(cindex->m_ChunkBlockIndexes[i], cindex2->m_ChunkBlockIndexes[i]);
        ASSERT_EQ(cindex->m_ChunkBlockOffsets[i], cindex2->m_ChunkBlockOffsets[i]);
        ASSERT_EQ(cindex->m_ChunkLengths[i], cindex2->m_ChunkLengths[i]);
    }

    Longtail_Free(cindex);
    cindex = 0;

    Longtail_Free(cindex2);
    cindex2 = 0;

    DestroyJobAPI(job_api);
    DestroyHashAPI(hash_api);
    DestroyStorageAPI(local_storage);
}

TEST(Longtail, WriteContent)
{
    StorageAPI* source_storage = CreateInMemStorageAPI();
    StorageAPI* target_storage = CreateInMemStorageAPI();
    CompressionRegistry* compression_registry = CreateDefaultCompressionRegistry();
    HashAPI* hash_api = CreateMeowHashAPI();
    JobAPI* job_api = CreateBikeshedJobAPI(0);

    const char* TEST_FILENAMES[5] = {
        "local/TheLongFile.txt",
        "local/ShortString.txt",
        "local/AnotherSample.txt",
        "local/folder/ShortString.txt",
        "local/AlsoShortString.txt"
    };

    const char* TEST_STRINGS[5] = {
        "This is the first test string which is fairly long and should - reconstructed properly, than you very much",
        "Short string",
        "Another sample string that does not match any other string but -reconstructed properly, than you very much",
        "Short string",
        "Short string"
    };

    for (uint32_t i = 0; i < 5; ++i)
    {
        ASSERT_NE(0, CreateParentPath(source_storage, TEST_FILENAMES[i]));
        StorageAPI_HOpenFile w;
        ASSERT_EQ(0, source_storage->OpenWriteFile(source_storage, TEST_FILENAMES[i], 0, &w));
        ASSERT_NE((StorageAPI_HOpenFile)0, w);
        ASSERT_EQ(0, source_storage->Write(source_storage, w, 0, strlen(TEST_STRINGS[i]) + 1, TEST_STRINGS[i]));
        source_storage->CloseFile(source_storage, w);
        w = 0;
    }

    FileInfos* version1_paths;
    ASSERT_EQ(0, GetFilesRecursively(source_storage, "local", &version1_paths));
    ASSERT_NE((FileInfos*)0, version1_paths);
    uint32_t* compression_types = GetCompressionTypes(source_storage, version1_paths);
    ASSERT_NE((uint32_t*)0, compression_types);
    VersionIndex* vindex = CreateVersionIndex(
        source_storage,
        hash_api,
        job_api,
        0,
        0,
        "local",
        &version1_paths->m_Paths,
        version1_paths->m_FileSizes,
        compression_types,
        16);
    ASSERT_NE((VersionIndex*)0, vindex);
    Longtail_Free(compression_types);
    compression_types = 0;
    Longtail_Free(version1_paths);
    version1_paths = 0;

    static const uint32_t MAX_BLOCK_SIZE = 32;
    static const uint32_t MAX_CHUNKS_PER_BLOCK = 3;
    ContentIndex* cindex = CreateContentIndex(
        hash_api,
        *vindex->m_ChunkCount,
        vindex->m_ChunkHashes,
        vindex->m_ChunkSizes,
        vindex->m_ChunkCompressionTypes,
        MAX_BLOCK_SIZE,
        MAX_CHUNKS_PER_BLOCK);
    ASSERT_NE((ContentIndex*)0, cindex);

    ASSERT_NE(0, WriteContent(
        source_storage,
        target_storage,
        compression_registry,
        job_api,
        0,
        0,
        cindex,
        vindex,
        "local",
        "chunks"));

    ContentIndex* cindex2 = ReadContent(
        target_storage,
        hash_api,
        job_api,
        0,
        0,
        "chunks");
    ASSERT_NE((ContentIndex*)0, cindex2);

    ASSERT_EQ(*cindex->m_BlockCount, *cindex2->m_BlockCount);
    for (uint64_t i = 0; i < *cindex->m_BlockCount; ++i)
    {
        uint64_t i2 = 0;
        while (cindex->m_BlockHashes[i] != cindex2->m_BlockHashes[i2])
        {
            ++i2;
            ASSERT_NE(i2, *cindex2->m_BlockCount);
        }
        ASSERT_EQ(cindex->m_BlockHashes[i], cindex2->m_BlockHashes[i2]);
    }
    ASSERT_EQ(*cindex->m_ChunkCount, *cindex2->m_ChunkCount);
    for (uint64_t i = 0; i < *cindex->m_ChunkCount; ++i)
    {
        uint64_t i2 = 0;
        while (cindex->m_ChunkHashes[i] != cindex2->m_ChunkHashes[i2])
        {
            ++i2;
            ASSERT_NE(i2, *cindex2->m_ChunkCount);
        }
        ASSERT_EQ(cindex->m_BlockHashes[cindex->m_ChunkBlockIndexes[i]], cindex2->m_BlockHashes[cindex2->m_ChunkBlockIndexes[i2]]);
        ASSERT_EQ(cindex->m_ChunkBlockOffsets[i], cindex2->m_ChunkBlockOffsets[i2]);
        ASSERT_EQ(cindex->m_ChunkLengths[i], cindex2->m_ChunkLengths[i2]);
    }

    Longtail_Free(cindex2);
    Longtail_Free(cindex);
    Longtail_Free(vindex);

    DestroyJobAPI(job_api);
    DestroyHashAPI(hash_api);
    DestroyCompressionRegistry(compression_registry);
    DestroyStorageAPI(target_storage);
    DestroyStorageAPI(source_storage);
}

#if 0
TEST(Longtail, TestVeryLargeFile)
{
    const char* assets_path = "C:\\Temp\\longtail\\local\\WinClient\\CL6332_WindowsClient\\WindowsClient\\PioneerGame\\Content\\Paks";

    FileInfos* paths = GetFilesRecursively(storage_api, assets_path);
    VersionIndex* version_index = CreateVersionIndex(
        storage_api,
        hash_api,
        job_api,
        0,
        0,
        assets_path,
        &paths->m_Paths,
        paths->m_FileSizes,
        32758u);

    Longtail_Free(version_index);
}
#endif // 0

TEST(Longtail, DiffHashes)
{

//void DiffHashes(const TLongtail_Hash* reference_hashes, uint32_t reference_hash_count, const TLongtail_Hash* new_hashes, uint32_t new_hash_count, uint32_t* added_hash_count, TLongtail_Hash* added_hashes, uint32_t* removed_hash_count, TLongtail_Hash* removed_hashes)
}

TEST(Longtail, GetUniqueAssets)
{
//uint32_t GetUniqueAssets(uint64_t asset_count, const TLongtail_Hash* asset_content_hashes, uint32_t* out_unique_asset_indexes)

}

TEST(Longtail, GetBlocksForAssets)
{
//    ContentIndex* GetBlocksForAssets(const ContentIndex* content_index, uint64_t asset_count, const TLongtail_Hash* asset_hashes, uint64_t* out_missing_asset_count, uint64_t* out_missing_assets)
}

TEST(Longtail, CreateMissingContent)
{
    HashAPI* hash_api = CreateMeowHashAPI();

    const char* assets_path = "";
    const uint64_t asset_count = 5;
    const TLongtail_Hash asset_content_hashes[5] = { 5, 4, 3, 2, 1};
    const TLongtail_Hash asset_path_hashes[5] = {50, 40, 30, 20, 10};
    const uint64_t asset_sizes[5] = {43593, 43593, 43592, 43591, 43591};
    const uint32_t chunk_sizes[5] = {43593, 43593, 43592, 43591, 43591};
    const uint32_t asset_name_offsets[5] = { 7 * 0, 7 * 1, 7 * 2, 7 * 3, 7 * 4};
    const char* asset_name_data = { "fifth_\0" "fourth\0" "third_\0" "second\0" "first_\0" };
    const uint32_t asset_chunk_counts[5] = {1, 1, 1, 1, 1};
    const uint32_t asset_chunk_start_index[5] = {0, 1, 2, 3, 4};
    const uint32_t asset_compression_types[5] = {0, 0, 0, 0, 0};

    static const uint32_t MAX_BLOCK_SIZE = 65536 * 2;
    static const uint32_t MAX_CHUNKS_PER_BLOCK = 4096;
    ContentIndex* content_index = CreateContentIndex(
        hash_api,
        asset_count - 4,
        asset_content_hashes,
        chunk_sizes,
        asset_compression_types,
        MAX_BLOCK_SIZE,
        MAX_CHUNKS_PER_BLOCK);

    const char* asset_paths[5] = {
        "fifth_",
        "fourth",
        "third_",
        "second",
        "first_"
    };

    Paths* paths = MakePaths(5, asset_paths);
    size_t version_index_size = GetVersionIndexSize(5, 5, 5, paths->m_DataSize);
    void* version_index_mem = Longtail_Alloc(version_index_size);

    VersionIndex* version_index = BuildVersionIndex(
        version_index_mem,
        version_index_size,
        paths,
        asset_path_hashes,
        asset_content_hashes,
        asset_sizes,
        asset_chunk_start_index,
        asset_chunk_counts,
        *paths->m_PathCount,
        asset_chunk_start_index,
        *paths->m_PathCount,
        chunk_sizes,
        asset_content_hashes,
        asset_compression_types);
    Longtail_Free(paths);

    ContentIndex* missing_content_index = CreateMissingContent(
        hash_api,
        content_index,
        version_index,
        MAX_BLOCK_SIZE,
        MAX_CHUNKS_PER_BLOCK);

    ASSERT_EQ(2u, *missing_content_index->m_BlockCount);
    ASSERT_EQ(4u, *missing_content_index->m_ChunkCount);

    ASSERT_EQ(0u, missing_content_index->m_ChunkBlockIndexes[0]);
    ASSERT_EQ(asset_content_hashes[4], missing_content_index->m_ChunkHashes[0]);
    ASSERT_EQ(asset_sizes[4], missing_content_index->m_ChunkLengths[0]);

    ASSERT_EQ(0u, missing_content_index->m_ChunkBlockIndexes[0]);
    ASSERT_EQ(asset_content_hashes[3], missing_content_index->m_ChunkHashes[1]);
    ASSERT_EQ(asset_sizes[3], missing_content_index->m_ChunkLengths[1]);
    ASSERT_EQ(43591, missing_content_index->m_ChunkBlockOffsets[1]);

    ASSERT_EQ(0u, missing_content_index->m_ChunkBlockIndexes[2]);
    ASSERT_EQ(asset_content_hashes[2], missing_content_index->m_ChunkHashes[2]);
    ASSERT_EQ(asset_sizes[2], missing_content_index->m_ChunkLengths[2]);
    ASSERT_EQ(43591 * 2, missing_content_index->m_ChunkBlockOffsets[2]);

    ASSERT_EQ(1u, missing_content_index->m_ChunkBlockIndexes[3]);
    ASSERT_EQ(asset_content_hashes[1], missing_content_index->m_ChunkHashes[3]);
    ASSERT_EQ(asset_sizes[1], missing_content_index->m_ChunkLengths[3]);
    ASSERT_EQ(0u, missing_content_index->m_ChunkBlockOffsets[3]);

    Longtail_Free(version_index);
    Longtail_Free(content_index);

    Longtail_Free(missing_content_index);

    DestroyHashAPI(hash_api);
}

TEST(Longtail, GetMissingAssets)
{
//    uint64_t GetMissingAssets(const ContentIndex* content_index, const VersionIndex* version, TLongtail_Hash* missing_assets)
}

TEST(Longtail, VersionIndexDirectories)
{
    StorageAPI* local_storage = CreateInMemStorageAPI();
    HashAPI* hash_api = CreateMeowHashAPI();
    JobAPI* job_api = CreateBikeshedJobAPI(0);

    ASSERT_EQ(1, CreateFakeContent(local_storage, "two_items", 2));
    ASSERT_EQ(0, local_storage->CreateDir(local_storage, "no_items"));
    ASSERT_EQ(1, CreateFakeContent(local_storage, "deep/file/down/under/three_items", 3));
    ASSERT_EQ(1, MakePath(local_storage, "deep/folders/with/nothing/in/menoexists.nop"));

    FileInfos* local_paths;
    ASSERT_EQ(0, GetFilesRecursively(local_storage, "", &local_paths));
    ASSERT_NE((FileInfos*)0, local_paths);
    uint32_t* compression_types = GetCompressionTypes(local_storage, local_paths);
    ASSERT_NE((uint32_t*)0, compression_types);

    VersionIndex* local_version_index = CreateVersionIndex(
        local_storage,
        hash_api,
        job_api,
        0,
        0,
        "",
        &local_paths->m_Paths,
        local_paths->m_FileSizes,
        compression_types,
        16384);
    ASSERT_NE((VersionIndex*)0, local_version_index);
    ASSERT_EQ(16, *local_version_index->m_AssetCount);

    Longtail_Free(compression_types);
    Longtail_Free(local_version_index);
    Longtail_Free(local_paths);

    DestroyHashAPI(hash_api);
    DestroyJobAPI(job_api);
    DestroyStorageAPI(local_storage);
}

TEST(Longtail, MergeContentIndex)
{
    HashAPI* hash_api = CreateMeowHashAPI();
    ContentIndex* cindex1 = CreateContentIndex(
        hash_api,
        0,
        0,
        0,
        0,
        16,
        8);
    ASSERT_NE((ContentIndex*)0, cindex1);
    ContentIndex* cindex2 = CreateContentIndex(
        hash_api,
        0,
        0,
        0,
        0,
        16,
        8);
    ASSERT_NE((ContentIndex*)0, cindex2);
    ContentIndex* cindex3 = MergeContentIndex(cindex1, cindex2);
    ASSERT_NE((ContentIndex*)0, cindex3);

    TLongtail_Hash chunk_hashes_4[] = {5, 6, 7};
    uint32_t chunk_sizes_4[] = {10, 20, 10};
    uint32_t chunk_compression_types_4[] = {0, 0, 0};
    ContentIndex* cindex4 = CreateContentIndex(
        hash_api,
        3,
        chunk_hashes_4,
        chunk_sizes_4,
        chunk_compression_types_4,
        30,
        2);
    ASSERT_NE((ContentIndex*)0, cindex4);

    TLongtail_Hash chunk_hashes_5[] = {8, 7, 6};
    uint32_t chunk_sizes_5[] = {20, 10, 20};
    uint32_t chunk_compression_types_5[] = {0, 0, 0};

    ContentIndex* cindex5 = CreateContentIndex(
        hash_api,
        3,
        chunk_hashes_5,
        chunk_sizes_5,
        chunk_compression_types_5,
        30,
        2);
    ASSERT_NE((ContentIndex*)0, cindex5);

    ContentIndex* cindex6 = MergeContentIndex(cindex4, cindex5);
    ASSERT_NE((ContentIndex*)0, cindex6);
    ASSERT_EQ(4, *cindex6->m_BlockCount);
    ASSERT_EQ(6, *cindex6->m_ChunkCount);

    ContentIndex* cindex7 = MergeContentIndex(cindex6, cindex1);
    ASSERT_NE((ContentIndex*)0, cindex7);
    ASSERT_EQ(4, *cindex7->m_BlockCount);
    ASSERT_EQ(6, *cindex7->m_ChunkCount);

    Longtail_Free(cindex7);
    Longtail_Free(cindex6);
    Longtail_Free(cindex5);
    Longtail_Free(cindex4);
    Longtail_Free(cindex3);
    Longtail_Free(cindex2);
    Longtail_Free(cindex1);

    DestroyHashAPI(hash_api);
}

TEST(Longtail, VersionDiff)
{
    StorageAPI* storage = CreateInMemStorageAPI();
    CompressionRegistry* compression_registry = CreateDefaultCompressionRegistry();
    HashAPI* hash_api = CreateMeowHashAPI();
    JobAPI* job_api = CreateBikeshedJobAPI(0);

    const uint32_t OLD_ASSET_COUNT = 9;

    const char* OLD_TEST_FILENAMES[] = {
        "ContentChangedSameLength.txt",
        "WillBeRenamed.txt",
        "ContentSameButShorter.txt",
        "folder/ContentSameButLonger.txt",
        "OldRenamedFolder/MovedToNewFolder.txt",
        "JustDifferent.txt",
        "EmptyFileInFolder/.init.py",
        "a/file/in/folder/LongWithChangedStart.dll",
        "a/file/in/other/folder/LongChangedAtEnd.exe"
    };

    const char* OLD_TEST_STRINGS[] = {
        "This is the first test string which is fairly long and should - reconstructed properly, than you very much",
        "Short string",
        "Another sample string that does not match any other string but -reconstructed properly, than you very much",
        "Short string",
        "This is the documentation we are all craving to understand the complexities of life",
        "More than chunk less than block",
        "",
        "A very long file that should be able to be recreated"
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 2 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 3 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 4 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 5 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 6 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 7 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 8 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 9 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 10 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 11 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 12 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 13 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 14 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 15 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 16 in a long sequence of stuff."
            "And in the end it is not the same, it is different, just because why not",
        "A very long file that should be able to be recreated"
            "Another big file but this does not contain the data as the one above, however it does start out the same as the other file,right?"
            "Yet we also repeat this line, this is the first time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the second time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the third time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the fourth time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the fifth time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the sixth time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the eigth time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the ninth time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the tenth time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the elevth time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the twelth time you see this, but it will also show up again and again with only small changes"
            "I realize I'm not very good at writing out the numbering with the 'th stuff at the end. Not much reason to use that before."
            "0123456789876543213241247632464358091345+2438568736283249873298ntyvntrndwoiy78n43ctyermdr498xrnhse78tnls43tc49mjrx3hcnthv4t"
            "liurhe ngvh43oecgclri8fhso7r8ab3gwc409nu3p9t757nvv74oe8nfyiecffömocsrhf ,jsyvblse4tmoxw3umrc9sen8tyn8öoerucdlc4igtcov8evrnocs8lhrf"
            "That will look like garbage, will that really be a good idea?"
            "This is the end tough..."
    };

    const size_t OLD_TEST_SIZES[] = {
        strlen(OLD_TEST_STRINGS[0]) + 1,
        strlen(OLD_TEST_STRINGS[1]) + 1,
        strlen(OLD_TEST_STRINGS[2]) + 1,
        strlen(OLD_TEST_STRINGS[3]) + 1,
        strlen(OLD_TEST_STRINGS[4]) + 1,
        strlen(OLD_TEST_STRINGS[5]) + 1,
        strlen(OLD_TEST_STRINGS[6]) + 1,
        strlen(OLD_TEST_STRINGS[7]) + 1,
        strlen(OLD_TEST_STRINGS[8]) + 1,
        0
    };

    const uint32_t NEW_ASSET_COUNT = 9;

    const char* NEW_TEST_FILENAMES[] = {
        "ContentChangedSameLength.txt",
        "HasBeenRenamed.txt",
        "ContentSameButShorter.txt",
        "folder/ContentSameButLonger.txt",
        "NewRenamedFolder/MovedToNewFolder.txt",
        "JustDifferent.txt",
        "EmptyFileInFolder/.init.py",
        "a/file/in/folder/LongWithChangedStart.dll",
        "a/file/in/other/folder/LongChangedAtEnd.exe"
    };

    const char* NEW_TEST_STRINGS[] = {
        "This is the first test string which is fairly long and *will* - reconstructed properly, than you very much",   // Content changed, same length
        "Short string", // Renamed
        "Another sample string that does not match any other string but -reconstructed properly.",   // Shorter, same base content
        "Short string but with added stuff",    // Longer, same base content
        "This is the documentation we are all craving to understand the complexities of life",  // Same but moved to different folder
        "I wish I was a baller.", // Just different
        "", // Unchanged
        "!A very long file that should be able to be recreated"
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 2 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 3 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 4 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 5 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 6 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 7 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 8 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 9 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 10 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 11 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 12 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 13 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 14 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 15 in a long sequence of stuff."
            "Lots of repeating stuff, some good, some bad but still it is repeating. This is the number 16 in a long sequence of stuff.",
            "And in the end it is not the same, it is different, just because why not"  // Modified at start
        "A very long file that should be able to be recreated"
            "Another big file but this does not contain the data as the one above, however it does start out the same as the other file,right?"
            "Yet we also repeat this line, this is the first time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the second time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the third time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the fourth time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the fifth time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the sixth time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the eigth time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the ninth time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the tenth time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the elevth time you see this, but it will also show up again and again with only small changes"
            "Yet we also repeat this line, this is the twelth time you see this, but it will also show up again and again with only small changes"
            "I realize I'm not very good at writing out the numbering with the 'th stuff at the end. Not much reason to use that before."
            "0123456789876543213241247632464358091345+2438568736283249873298ntyvntrndwoiy78n43ctyermdr498xrnhse78tnls43tc49mjrx3hcnthv4t"
            "liurhe ngvh43oecgclri8fhso7r8ab3gwc409nu3p9t757nvv74oe8nfyiecffömocsrhf ,jsyvblse4tmoxw3umrc9sen8tyn8öoerucdlc4igtcov8evrnocs8lhrf"
            "That will look like garbage, will that really be a good idea?"
            "This is the end tough...!"  // Modified at start
    };

    const size_t NEW_TEST_SIZES[] = {
        strlen(NEW_TEST_STRINGS[0]) + 1,
        strlen(NEW_TEST_STRINGS[1]) + 1,
        strlen(NEW_TEST_STRINGS[2]) + 1,
        strlen(NEW_TEST_STRINGS[3]) + 1,
        strlen(NEW_TEST_STRINGS[4]) + 1,
        strlen(NEW_TEST_STRINGS[5]) + 1,
        strlen(NEW_TEST_STRINGS[6]) + 1,
        strlen(NEW_TEST_STRINGS[7]) + 1,
        strlen(NEW_TEST_STRINGS[8]) + 1,
        0
    };

    for (uint32_t i = 0; i < OLD_ASSET_COUNT; ++i)
    {
        char* file_name = storage->ConcatPath(storage, "old", OLD_TEST_FILENAMES[i]);
        ASSERT_NE(0, CreateParentPath(storage, file_name));
        StorageAPI_HOpenFile w;
        ASSERT_EQ(0, storage->OpenWriteFile(storage, file_name, 0, &w));
        Longtail_Free(file_name);
        ASSERT_NE((StorageAPI_HOpenFile)0, w);
        if (OLD_TEST_SIZES[i])
        {
            ASSERT_EQ(0, storage->Write(storage, w, 0, OLD_TEST_SIZES[i], OLD_TEST_STRINGS[i]));
        }
        storage->CloseFile(storage, w);
        w = 0;
    }

    for (uint32_t i = 0; i < NEW_ASSET_COUNT; ++i)
    {
        char* file_name = storage->ConcatPath(storage, "new", NEW_TEST_FILENAMES[i]);
        ASSERT_NE(0, CreateParentPath(storage, file_name));
        StorageAPI_HOpenFile w;
        ASSERT_EQ(0, storage->OpenWriteFile(storage, file_name, 0, &w));
        Longtail_Free(file_name);
        ASSERT_NE((StorageAPI_HOpenFile)0, w);
        if (NEW_TEST_SIZES[i])
        {
            ASSERT_EQ(0, storage->Write(storage, w, 0, NEW_TEST_SIZES[i], NEW_TEST_STRINGS[i]));
        }
        storage->CloseFile(storage, w);
        w = 0;
    }

    FileInfos* old_version_paths;
    ASSERT_EQ(0, GetFilesRecursively(storage, "old", &old_version_paths));
    ASSERT_NE((FileInfos*)0, old_version_paths);
    uint32_t* old_compression_types = GetCompressionTypes(storage, old_version_paths);
    ASSERT_NE((uint32_t*)0, old_compression_types);
    VersionIndex* old_vindex = CreateVersionIndex(
        storage,
        hash_api,
        job_api,
        0,
        0,
        "old",
        &old_version_paths->m_Paths,
        old_version_paths->m_FileSizes,
        old_compression_types,
        16);
    ASSERT_NE((VersionIndex*)0, old_vindex);
    Longtail_Free(old_compression_types);
    old_compression_types = 0;
    Longtail_Free(old_version_paths);
    old_version_paths = 0;

    FileInfos* new_version_paths;
    ASSERT_EQ(0, GetFilesRecursively(storage, "new", &new_version_paths));
    ASSERT_NE((FileInfos*)0, new_version_paths);
    uint32_t* new_compression_types = GetCompressionTypes(storage, new_version_paths);
    ASSERT_NE((uint32_t*)0, new_compression_types);
    VersionIndex* new_vindex = CreateVersionIndex(
        storage,
        hash_api,
        job_api,
        0,
        0,
        "new",
        &new_version_paths->m_Paths,
        new_version_paths->m_FileSizes,
        new_compression_types,
        16);
    ASSERT_NE((VersionIndex*)0, new_vindex);
    Longtail_Free(new_compression_types);
    new_compression_types = 0;
    Longtail_Free(new_version_paths);
    new_version_paths = 0;

    static const uint32_t MAX_BLOCK_SIZE = 32;
    static const uint32_t MAX_CHUNKS_PER_BLOCK = 3;

    ContentIndex* content_index = CreateContentIndex(
            hash_api,
            *new_vindex->m_ChunkCount,
            new_vindex->m_ChunkHashes,
            new_vindex->m_ChunkSizes,
            new_vindex->m_ChunkCompressionTypes,
            MAX_BLOCK_SIZE,
            MAX_CHUNKS_PER_BLOCK);

    ASSERT_EQ(1, WriteContent(
        storage,
        storage,
        compression_registry,
        job_api,
        0,
        0,
        content_index,
        new_vindex,
        "new",
        "chunks"));

    VersionDiff* version_diff = CreateVersionDiff(
        old_vindex,
        new_vindex);
    ASSERT_NE((VersionDiff*)0, version_diff);

    ASSERT_EQ(3, *version_diff->m_SourceRemovedCount);
    ASSERT_EQ(3, *version_diff->m_TargetAddedCount);
    ASSERT_EQ(6, *version_diff->m_ModifiedCount);

    ASSERT_NE(0, ChangeVersion(
        storage,
        storage,
        hash_api,
        job_api,
        0,
        0,
        compression_registry,
        content_index,
        old_vindex,
        new_vindex,
        version_diff,
        "chunks",
        "old"));

    Longtail_Free(content_index);

    Longtail_Free(version_diff);

    Longtail_Free(new_vindex);
    Longtail_Free(old_vindex);

    // Verify that our old folder now matches the new folder data
    FileInfos* updated_version_paths;
    ASSERT_EQ(0, GetFilesRecursively(storage, "old", &updated_version_paths));
    ASSERT_NE((FileInfos*)0, updated_version_paths);
    const uint32_t NEW_ASSET_FOLDER_EXTRA_COUNT = 10;
    ASSERT_EQ(NEW_ASSET_COUNT + NEW_ASSET_FOLDER_EXTRA_COUNT, *updated_version_paths->m_Paths.m_PathCount);
    Longtail_Free(updated_version_paths);

    for (uint32_t i = 0; i < NEW_ASSET_COUNT; ++i)
    {
        char* file_name = storage->ConcatPath(storage, "old", NEW_TEST_FILENAMES[i]);
        StorageAPI_HOpenFile r;
        ASSERT_EQ(0, storage->OpenReadFile(storage, file_name, &r));
        Longtail_Free(file_name);
        ASSERT_NE((StorageAPI_HOpenFile)0, r);
        uint64_t size;
        ASSERT_EQ(0, storage->GetSize(storage, r, &size));
        ASSERT_EQ(NEW_TEST_SIZES[i], size);
        char* test_data = (char*)Longtail_Alloc(sizeof(char) * size);
        if (size)
        {
            ASSERT_EQ(0, storage->Read(storage, r, 0, size, test_data));
            ASSERT_STREQ(NEW_TEST_STRINGS[i], test_data);
        }
        storage->CloseFile(storage, r);
        r = 0;
        Longtail_Free(test_data);
        test_data = 0;
    }

    DestroyJobAPI(job_api);
    DestroyHashAPI(hash_api);
    DestroyCompressionRegistry(compression_registry);
    DestroyStorageAPI(storage);
}

TEST(Longtail, FullScale)
{
    return;
    StorageAPI* local_storage = CreateInMemStorageAPI();
    CompressionRegistry* compression_registry = CreateDefaultCompressionRegistry();
    HashAPI* hash_api = CreateMeowHashAPI();
    JobAPI* job_api = CreateBikeshedJobAPI(0);

    CreateFakeContent(local_storage, 0, 5);

    StorageAPI* remote_storage = CreateInMemStorageAPI();
    CreateFakeContent(remote_storage, 0, 10);

    FileInfos* local_paths;
    ASSERT_EQ(0, GetFilesRecursively(local_storage, "", &local_paths));
    ASSERT_NE((FileInfos*)0, local_paths);
    uint32_t* local_compression_types = GetCompressionTypes(local_storage, local_paths);
    ASSERT_NE((uint32_t*)0, local_compression_types);

    VersionIndex* local_version_index = CreateVersionIndex(
        local_storage,
        hash_api,
        job_api,
        0,
        0,
        "",
        &local_paths->m_Paths,
        local_paths->m_FileSizes,
        local_compression_types,
        16384);
    ASSERT_NE((VersionIndex*)0, local_version_index);
    ASSERT_EQ(5, *local_version_index->m_AssetCount);
    Longtail_Free(local_compression_types);
    local_compression_types = 0;

    FileInfos* remote_paths;
    ASSERT_EQ(0, GetFilesRecursively(remote_storage, "", &remote_paths));
    ASSERT_NE((FileInfos*)0, local_paths);
    uint32_t* remote_compression_types = GetCompressionTypes(local_storage, remote_paths);
    ASSERT_NE((uint32_t*)0, remote_compression_types);
    VersionIndex* remote_version_index = CreateVersionIndex(
        remote_storage,
        hash_api,
        job_api,
        0,
        0,
        "",
        &remote_paths->m_Paths,
        remote_paths->m_FileSizes,
        remote_compression_types,
        16384);
    ASSERT_NE((VersionIndex*)0, remote_version_index);
    ASSERT_EQ(10, *remote_version_index->m_AssetCount);
    Longtail_Free(remote_compression_types);
    remote_compression_types = 0;

    static const uint32_t MAX_BLOCK_SIZE = 65536 * 2;
    static const uint32_t MAX_CHUNKS_PER_BLOCK = 4096;

    ContentIndex* local_content_index = CreateContentIndex(
            hash_api,
            * local_version_index->m_ChunkCount,
            local_version_index->m_ChunkHashes,
            local_version_index->m_ChunkSizes,
            local_version_index->m_ChunkCompressionTypes,
            MAX_BLOCK_SIZE,
            MAX_CHUNKS_PER_BLOCK);

    ASSERT_EQ(1, WriteContent(
        local_storage,
        local_storage,
        compression_registry,
        job_api,
        0,
        0,
        local_content_index,
        local_version_index,
        "",
        ""));

    ContentIndex* remote_content_index = CreateContentIndex(
            hash_api,
            * remote_version_index->m_ChunkCount,
            remote_version_index->m_ChunkHashes,
            remote_version_index->m_ChunkSizes,
            remote_version_index->m_ChunkCompressionTypes,
            MAX_BLOCK_SIZE,
            MAX_CHUNKS_PER_BLOCK);

    ASSERT_EQ(1, WriteContent(
        remote_storage,
        remote_storage,
        compression_registry,
        job_api,
        0,
        0,
        remote_content_index,
        remote_version_index,
        "",
        ""));

    ContentIndex* missing_content = CreateMissingContent(
        hash_api,
        local_content_index,
        remote_version_index,
        MAX_BLOCK_SIZE,
        MAX_CHUNKS_PER_BLOCK);
    ASSERT_NE((ContentIndex*)0, missing_content);
 
    ASSERT_EQ(1, WriteContent(
        remote_storage,
        local_storage,
        compression_registry,
        job_api,
        0,
        0,
        missing_content,
        remote_version_index,
        "",
        ""));

    ContentIndex* merged_content_index = MergeContentIndex(local_content_index, missing_content);
    ASSERT_EQ(1, WriteVersion(
        local_storage,
        local_storage,
        compression_registry,
        job_api,
        0,
        0,
        merged_content_index,
        remote_version_index,
        "",
        ""));

    for(uint32_t i = 0; i < 10; ++i)
    {
        char path[20];
        sprintf(path, "%u", i);
        StorageAPI_HOpenFile r;
        ASSERT_EQ(0, local_storage->OpenReadFile(local_storage, path, &r));
        ASSERT_NE((StorageAPI_HOpenFile)0, r);
        uint64_t size;
        ASSERT_EQ(0, local_storage->GetSize(local_storage, r, &size));
        uint64_t expected_size = 64000 + 1 + i;
        ASSERT_EQ(expected_size, size);
        char* buffer = (char*)Longtail_Alloc(expected_size);
        ASSERT_EQ(0, local_storage->Read(local_storage, r, 0, expected_size, buffer));

        for (uint64_t j = 0; j < expected_size; j++)
        {
            ASSERT_EQ((int)i, (int)buffer[j]);
        }

        Longtail_Free(buffer);
        local_storage->CloseFile(local_storage, r);
    }

    Longtail_Free(missing_content);
    Longtail_Free(merged_content_index);
    Longtail_Free(remote_content_index);
    Longtail_Free(local_content_index);
    Longtail_Free(remote_version_index);
    Longtail_Free(local_version_index);

    DestroyJobAPI(job_api);
    DestroyHashAPI(hash_api);
    DestroyCompressionRegistry(compression_registry);
    DestroyStorageAPI(local_storage);
}


TEST(Longtail, WriteVersion)
{
    StorageAPI* storage_api = CreateInMemStorageAPI();
    CompressionRegistry* compression_registry = CreateDefaultCompressionRegistry();
    HashAPI* hash_api = CreateMeowHashAPI();
    JobAPI* job_api = CreateBikeshedJobAPI(0);

    const uint32_t asset_count = 8;

    const char* TEST_FILENAMES[] = {
        "TheLongFile.txt",
        "ShortString.txt",
        "AnotherSample.txt",
        "folder/ShortString.txt",
        "WATCHIOUT.txt",
        "empty/.init.py",
        "TheVeryLongFile.txt",
        "AnotherVeryLongFile.txt"
    };

    const char* TEST_STRINGS[] = {
        "This is the first test string which is fairly long and should - reconstructed properly, than you very much",
        "Short string",
        "Another sample string that does not match any other string but -reconstructed properly, than you very much",
        "Short string",
        "More than chunk less than block",
        "",
        "A very long string that should go over multiple blocks so we can test our super funky multi-threading version"
            "restore function that spawns a bunch of decompress jobs and makes the writes to disc sequentially using dependecies"
            "so we write in good order but still use all our cores in a reasonable fashion. So this should be a long long string"
            "longer than seems reasonable, and here is a lot of rambling in this string. Because it is late and I just need to fill"
            "the string but make sure it actually comes back fine"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "this is the end...",
        "Another very long string that should go over multiple blocks so we can test our super funky multi-threading version"
            "restore function that spawns a bunch of decompress jobs and makes the writes to disc sequentially using dependecies"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "so we write in good order but still use all our cores in a reasonable fashion. So this should be a long long string"
            "longer than seems reasonable, and here is a lot of rambling in this string. Because it is late and I just need to fill"
            "the string but make sure it actually comes back fine"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "repeat, repeat, repeate, endless repeat, and some more repeat. You need more? Yes, repeat!"
            "this is the end..."
    };

    const size_t TEST_SIZES[] = {
        strlen(TEST_STRINGS[0]) + 1,
        strlen(TEST_STRINGS[1]) + 1,
        strlen(TEST_STRINGS[2]) + 1,
        strlen(TEST_STRINGS[3]) + 1,
        strlen(TEST_STRINGS[4]) + 1,
        0,
        strlen(TEST_STRINGS[6]) + 1,
        strlen(TEST_STRINGS[7]) + 1
    };

    for (uint32_t i = 0; i < asset_count; ++i)
    {
        char* file_name = storage_api->ConcatPath(storage_api, "local", TEST_FILENAMES[i]);
        ASSERT_NE(0, CreateParentPath(storage_api, file_name));
        StorageAPI_HOpenFile w;
        ASSERT_EQ(0, storage_api->OpenWriteFile(storage_api, file_name, 0, &w));
        Longtail_Free(file_name);
        ASSERT_NE((StorageAPI_HOpenFile)0, w);
        if (TEST_SIZES[i])
        {
            ASSERT_EQ(0, storage_api->Write(storage_api, w, 0, TEST_SIZES[i], TEST_STRINGS[i]));
        }
        storage_api->CloseFile(storage_api, w);
        w = 0;
    }

    FileInfos* version1_paths;
    ASSERT_EQ(0, GetFilesRecursively(storage_api, "local", &version1_paths));
    ASSERT_NE((FileInfos*)0, version1_paths);
    uint32_t* version1_compression_types = GetCompressionTypes(storage_api, version1_paths);
    ASSERT_NE((uint32_t*)0, version1_compression_types);
    VersionIndex* vindex = CreateVersionIndex(
        storage_api,
        hash_api,
        job_api,
        0,
        0,
        "local",
        &version1_paths->m_Paths,
        version1_paths->m_FileSizes,
        version1_compression_types,
        50);
    ASSERT_NE((VersionIndex*)0, vindex);
    Longtail_Free(version1_compression_types);
    version1_compression_types = 0;
    Longtail_Free(version1_paths);
    version1_paths = 0;

    static const uint32_t MAX_BLOCK_SIZE = 32;
    static const uint32_t MAX_CHUNKS_PER_BLOCK = 3;
    ContentIndex* cindex = CreateContentIndex(
        hash_api,
        *vindex->m_ChunkCount,
        vindex->m_ChunkHashes,
        vindex->m_ChunkSizes,
        vindex->m_ChunkCompressionTypes,
        MAX_BLOCK_SIZE,
        MAX_CHUNKS_PER_BLOCK);
    ASSERT_NE((ContentIndex*)0, cindex);

    ASSERT_NE(0, WriteContent(
        storage_api,
        storage_api,
        compression_registry,
        job_api,
        0,
        0,
        cindex,
        vindex,
        "local",
        "chunks"));

    ASSERT_NE(0, WriteVersion(
        storage_api,
        storage_api,
        compression_registry,
        job_api,
        0,
        0,
        cindex,
        vindex,
        "chunks",
        "remote"));

    for (uint32_t i = 0; i < asset_count; ++i)
    {
        char* file_name = storage_api->ConcatPath(storage_api, "remote", TEST_FILENAMES[i]);
        StorageAPI_HOpenFile r;
        ASSERT_EQ(0, storage_api->OpenReadFile(storage_api, file_name, &r));
        Longtail_Free(file_name);
        ASSERT_NE((StorageAPI_HOpenFile)0, r);
        uint64_t size;
        ASSERT_EQ(0, storage_api->GetSize(storage_api, r, &size));
        ASSERT_EQ(TEST_SIZES[i], size);
        char* test_data = (char*)Longtail_Alloc(sizeof(char) * size);
        if (size)
        {
            ASSERT_EQ(0, storage_api->Read(storage_api, r, 0, size, test_data));
            ASSERT_STREQ(TEST_STRINGS[i], test_data);
        }
        storage_api->CloseFile(storage_api, r);
        r = 0;
        Longtail_Free(test_data);
        test_data = 0;
    }

    Longtail_Free(vindex);
    vindex = 0;
    Longtail_Free(cindex);
    cindex = 0;
    DestroyJobAPI(job_api);
    DestroyHashAPI(hash_api);
    DestroyCompressionRegistry(compression_registry);
    DestroyStorageAPI(storage_api);
}

void Bench()
{
    if (1) return;


#if 0
    #define HOME "D:\\Temp\\longtail"

    const uint32_t VERSION_COUNT = 6;
    const char* VERSION[VERSION_COUNT] = {
        "git2f7f84a05fc290c717c8b5c0e59f8121481151e6_Win64_Editor",
        "git916600e1ecb9da13f75835cd1b2d2e6a67f1a92d_Win64_Editor",
        "gitfdeb1390885c2f426700ca653433730d1ca78dab_Win64_Editor",
        "git81cccf054b23a0b5a941612ef0a2a836b6e02fd6_Win64_Editor",
        "git558af6b2a10d9ab5a267b219af4f795a17cc032f_Win64_Editor",
        "gitc2ae7edeab85d5b8b21c8c3a29c9361c9f957f0c_Win64_Editor"
    };
#else
    #define HOME "C:\\Temp\\longtail"

    const uint32_t VERSION_COUNT = 2;
    const char* VERSION[VERSION_COUNT] = {
        "git75a99408249875e875f8fba52b75ea0f5f12a00e_Win64_Editor",
        "gitb1d3adb4adce93d0f0aa27665a52be0ab0ee8b59_Win64_Editor"
    };
#endif

    const char* SOURCE_VERSION_PREFIX = HOME "\\local\\";
    const char* VERSION_INDEX_SUFFIX = ".lvi";
    const char* CONTENT_INDEX_SUFFIX = ".lci";

    const char* CONTENT_FOLDER = HOME "\\chunks";

    const char* UPLOAD_VERSION_PREFIX = HOME "\\upload\\";
    const char* UPLOAD_VERSION_SUFFIX = "_chunks";

    const char* TARGET_VERSION_PREFIX = HOME "\\remote\\";

    struct StorageAPI* storage_api = CreateFSStorageAPI();
    CompressionRegistry* compression_registry = CreateDefaultCompressionRegistry();
    HashAPI* hash_api = CreateMeowHashAPI();
    JobAPI* job_api = CreateBikeshedJobAPI(0);

    static const uint32_t MAX_BLOCK_SIZE = 65536 * 2;
    static const uint32_t MAX_CHUNKS_PER_BLOCK = 4096;
    ContentIndex* full_content_index = CreateContentIndex(
            hash_api,
            0,
            0,
            0,
            0,
            MAX_BLOCK_SIZE,
            MAX_CHUNKS_PER_BLOCK);
    ASSERT_NE((ContentIndex*)0, full_content_index);
    VersionIndex* version_indexes[VERSION_COUNT];

    for (uint32_t i = 0; i < VERSION_COUNT; ++i)
    {
        char version_source_folder[256];
        sprintf(version_source_folder, "%s%s", SOURCE_VERSION_PREFIX, VERSION[i]);
        printf("Indexing `%s`\n", version_source_folder);
        FileInfos* version_source_paths;
        ASSERT_EQ(0, GetFilesRecursively(storage_api, version_source_folder, &version_source_paths));
        ASSERT_NE((FileInfos*)0, version_source_paths);
        uint32_t* version_compression_types = GetCompressionTypes(storage_api, version_source_paths);
        ASSERT_NE((uint32_t*)0, version_compression_types);
        VersionIndex* version_index = CreateVersionIndex(
            storage_api,
            hash_api,
            job_api,
            0,
            0,
            version_source_folder,
            &version_source_paths->m_Paths,
            version_source_paths->m_FileSizes,
            version_compression_types,
            16384);
        Longtail_Free(version_compression_types);
        version_compression_types = 0;
        Longtail_Free(version_source_paths);
        version_source_paths = 0;
        ASSERT_NE((VersionIndex*)0, version_index);
        printf("Indexed %u assets from `%s`\n", (uint32_t)*version_index->m_AssetCount, version_source_folder);

        char version_index_file[256];
        sprintf(version_index_file, "%s%s%s", SOURCE_VERSION_PREFIX, VERSION[i], VERSION_INDEX_SUFFIX);
        ASSERT_NE(0, WriteVersionIndex(storage_api, version_index, version_index_file));
        printf("Wrote version index to `%s`\n", version_index_file);

        ContentIndex* missing_content_index = CreateMissingContent(
            hash_api,
            full_content_index,
            version_index,
            MAX_BLOCK_SIZE,
            MAX_CHUNKS_PER_BLOCK);
        ASSERT_NE((ContentIndex*)0, missing_content_index);

        char delta_upload_content_folder[256];
        sprintf(delta_upload_content_folder, "%s%s%s", UPLOAD_VERSION_PREFIX, VERSION[i], UPLOAD_VERSION_SUFFIX);
        printf("Writing %" PRIu64 " block to `%s`\n", *missing_content_index->m_BlockCount, delta_upload_content_folder);
        ASSERT_NE(0, WriteContent(
            storage_api,
            storage_api,
            compression_registry,
            job_api,
            0,
            0,
            missing_content_index,
            version_index,
            version_source_folder,
            delta_upload_content_folder));

        printf("Copying %" PRIu64 " blocks from `%s` to `%s`\n", *missing_content_index->m_BlockCount, delta_upload_content_folder, CONTENT_FOLDER);
        StorageAPI_HIterator fs_iterator;
        int err = storage_api->StartFind(storage_api, delta_upload_content_folder, &fs_iterator);
        if (!err)
        {
            do
            {
                const char* file_name = storage_api->GetFileName(storage_api, fs_iterator);
                if (file_name)
                {
                    char* target_path = storage_api->ConcatPath(storage_api, CONTENT_FOLDER, file_name);

                    StorageAPI_HOpenFile v;
                    if (0 == storage_api->OpenReadFile(storage_api, target_path, &v))
                    {
                        storage_api->CloseFile(storage_api, v);
                        v = 0;
                        Longtail_Free(target_path);
                        continue;
                    }

                    char* source_path = storage_api->ConcatPath(storage_api, delta_upload_content_folder, file_name);

                    StorageAPI_HOpenFile s;
                    ASSERT_EQ(0, storage_api->OpenReadFile(storage_api, source_path, &s));
                    ASSERT_NE((StorageAPI_HOpenFile)0, s);

                    ASSERT_NE(0, MakePath(storage_api, target_path));
                    StorageAPI_HOpenFile t;
                    ASSERT_EQ(0, storage_api->OpenWriteFile(storage_api, target_path, 0, &t));
                    ASSERT_NE((StorageAPI_HOpenFile)0, t);

                    uint64_t block_file_size;
                    ASSERT_EQ(0, storage_api->GetSize(storage_api, s, &block_file_size));
                    void* buffer = Longtail_Alloc(block_file_size);

                    ASSERT_EQ(0, storage_api->Read(storage_api, s, 0, block_file_size, buffer));
                    ASSERT_EQ(0, storage_api->Write(storage_api, t, 0, block_file_size, buffer));

                    Longtail_Free(buffer);
                    buffer = 0,

                    storage_api->CloseFile(storage_api, s);
                    storage_api->CloseFile(storage_api, t);

                    Longtail_Free(target_path);
                    Longtail_Free(source_path);
                }
            }while(storage_api->FindNext(storage_api, fs_iterator) == 0);
        }
        else
        {
            ASSERT_EQ(ENOENT, err);
        }

        ContentIndex* merged_content_index = MergeContentIndex(full_content_index, missing_content_index);
        ASSERT_NE((ContentIndex*)0, merged_content_index);
        Longtail_Free(missing_content_index);
        missing_content_index = 0;
        Longtail_Free(full_content_index);
        full_content_index = merged_content_index;
        merged_content_index = 0;

        char version_target_folder[256];
        sprintf(version_target_folder, "%s%s", TARGET_VERSION_PREFIX, VERSION[i]);
        printf("Reconstructing %u assets from `%s` to `%s`\n", *version_index->m_AssetCount, CONTENT_FOLDER, version_target_folder);
        ASSERT_NE(0, WriteVersion(
            storage_api,
            storage_api,
            compression_registry,
            job_api,
            0,
            0,
            full_content_index,
            version_index,
            CONTENT_FOLDER,
            version_target_folder));

        version_indexes[i] = version_index;
        version_index = 0;
    }

    for (uint32_t i = 0; i < VERSION_COUNT; ++i)
    {
        Longtail_Free(version_indexes[i]);
    }

    Longtail_Free(full_content_index);

    DestroyJobAPI(job_api);
    DestroyHashAPI(hash_api);
    DestroyCompressionRegistry(compression_registry);
    DestroyStorageAPI(storage_api);

    #undef HOME
}

void LifelikeTest()
{
    if (1) return;

//    #define HOME "test\\data"
    #define HOME "D:\\Temp\\longtail"

    #define VERSION1 "75a99408249875e875f8fba52b75ea0f5f12a00e"
    #define VERSION2 "b1d3adb4adce93d0f0aa27665a52be0ab0ee8b59"

    #define VERSION1_FOLDER "git" VERSION1 "_Win64_Editor"
    #define VERSION2_FOLDER "git" VERSION2 "_Win64_Editor"

//    #define VERSION1_FOLDER "version1"
//    #define VERSION2_FOLDER "version2"

    const char* local_path_1 = HOME "\\local\\" VERSION1_FOLDER;
    const char* version_index_path_1 = HOME "\\local\\" VERSION1_FOLDER ".lvi";

    const char* local_path_2 = HOME "\\local\\" VERSION2_FOLDER;
    const char* version_index_path_2 = HOME "\\local\\" VERSION1_FOLDER ".lvi";

    const char* local_content_path = HOME "\\local_content";//HOME "\\local_content";
    const char* local_content_index_path = HOME "\\local.lci";

    const char* remote_content_path = HOME "\\remote_content";
    const char* remote_content_index_path = HOME "\\remote.lci";

    const char* remote_path_1 = HOME "\\remote\\" VERSION1_FOLDER;
    const char* remote_path_2 = HOME "\\remote\\" VERSION2_FOLDER;

    printf("Indexing `%s`...\n", local_path_1);
    struct StorageAPI* storage_api = CreateFSStorageAPI();
    CompressionRegistry* compression_registry = CreateDefaultCompressionRegistry();
    HashAPI* hash_api = CreateMeowHashAPI();
    JobAPI* job_api = CreateBikeshedJobAPI(0);

    FileInfos* local_path_1_paths;
    ASSERT_EQ(0, GetFilesRecursively(storage_api, local_path_1, &local_path_1_paths));
    ASSERT_NE((FileInfos*)0, local_path_1_paths);
    uint32_t* local_compression_types = GetCompressionTypes(storage_api, local_path_1_paths);
    ASSERT_NE((uint32_t*)0, local_compression_types);
    VersionIndex* version1 = CreateVersionIndex(
        storage_api,
        hash_api,
        job_api,
        0,
        0,
        local_path_1,
        &local_path_1_paths->m_Paths,
        local_path_1_paths->m_FileSizes,
        local_compression_types,
        16384);
    WriteVersionIndex(storage_api, version1, version_index_path_1);
    Longtail_Free(local_compression_types);
    local_compression_types = 0;
    Longtail_Free(local_path_1_paths);
    local_path_1_paths = 0;
    printf("%u assets from folder `%s` indexed to `%s`\n", *version1->m_AssetCount, local_path_1, version_index_path_1);

    printf("Creating local content index...\n");
    static const uint32_t MAX_BLOCK_SIZE = 65536 * 2;
    static const uint32_t MAX_CHUNKS_PER_BLOCK = 4096;
    ContentIndex* local_content_index = CreateContentIndex(
        hash_api,
        *version1->m_ChunkCount,
        version1->m_ChunkHashes,
        version1->m_ChunkSizes,
        version1->m_ChunkCompressionTypes,
        MAX_BLOCK_SIZE,
        MAX_CHUNKS_PER_BLOCK);

    printf("Writing local content index...\n");
    WriteContentIndex(storage_api, local_content_index, local_content_index_path);
    printf("%" PRIu64 " blocks from version `%s` indexed to `%s`\n", *local_content_index->m_BlockCount, local_path_1, local_content_index_path);

    if (1)
    {
        printf("Writing %" PRIu64 " block to `%s`\n", *local_content_index->m_BlockCount, local_content_path);
        WriteContent(
            storage_api,
            storage_api,
            compression_registry,
            job_api,
            0,
            0,
            local_content_index,
            version1,
            local_path_1,
            local_content_path);
    }

    printf("Reconstructing %u assets to `%s`\n", *version1->m_AssetCount, remote_path_1);
    ASSERT_EQ(1, WriteVersion(
        storage_api,
        storage_api,
        compression_registry,
        job_api,
        0,
        0,
        local_content_index,
        version1,
        local_content_path,
        remote_path_1));
    printf("Reconstructed %u assets to `%s`\n", *version1->m_AssetCount, remote_path_1);

    printf("Indexing `%s`...\n", local_path_2);
    FileInfos* local_path_2_paths;
    ASSERT_EQ(0, GetFilesRecursively(storage_api, local_path_2, &local_path_2_paths));
    ASSERT_NE((FileInfos*)0, local_path_2_paths);
    uint32_t* local_2_compression_types = GetCompressionTypes(storage_api, local_path_2_paths);
    ASSERT_NE((uint32_t*)0, local_2_compression_types);
    VersionIndex* version2 = CreateVersionIndex(
        storage_api,
        hash_api,
        job_api,
        0,
        0,
        local_path_2,
        &local_path_2_paths->m_Paths,
        local_path_2_paths->m_FileSizes,
        local_2_compression_types,
        16384);
    Longtail_Free(local_2_compression_types);
    local_2_compression_types = 0;
    Longtail_Free(local_path_2_paths);
    local_path_2_paths = 0;
    ASSERT_NE((VersionIndex*)0, version2);
    ASSERT_EQ(1, WriteVersionIndex(storage_api, version2, version_index_path_2));
    printf("%u assets from folder `%s` indexed to `%s`\n", *version2->m_AssetCount, local_path_2, version_index_path_2);
    
    // What is missing in local content that we need from remote version in new blocks with just the missing assets.
    ContentIndex* missing_content = CreateMissingContent(
        hash_api,
        local_content_index,
        version2,
        MAX_BLOCK_SIZE,
        MAX_CHUNKS_PER_BLOCK);
    ASSERT_NE((ContentIndex*)0, missing_content);
    printf("%" PRIu64 " blocks for version `%s` needed in content index `%s`\n", *missing_content->m_BlockCount, local_path_1, local_content_path);

    if (1)
    {
        printf("Writing %" PRIu64 " block to `%s`\n", *missing_content->m_BlockCount, local_content_path);
        ASSERT_EQ(1, WriteContent(
            storage_api,
            storage_api,
            compression_registry,
            job_api,
            0,
            0,
            missing_content,
            version2,
            local_path_2,
            local_content_path));
    }

    if (1)
    {
        // Write this to disk for reference to see how big the diff is...
        printf("Writing %" PRIu64 " block to `%s`\n", *missing_content->m_BlockCount, remote_content_path);
        ASSERT_EQ(1, WriteContent(
            storage_api,
            storage_api,
            compression_registry,
            job_api,
            0,
            0,
            missing_content,
            version2,
            local_path_2,
            remote_content_path));
    }

//    ContentIndex* remote_content_index = CreateContentIndex(
//        local_path_2,
//        *version2->m_AssetCount,
//        version2->m_ContentHashes,
//        version2->m_PathHashes,
//        version2->m_AssetSize,
//        version2->m_NameOffsets,
//        version2->m_NameData,
//        GetContentTag);

//    uint64_t* missing_assets = (uint64_t*)malloc(sizeof(uint64_t) * *version2->m_AssetCount);
//    uint64_t missing_asset_count = GetMissingAssets(local_content_index, version2, missing_assets);
//
//    uint64_t* remaining_missing_assets = (uint64_t*)malloc(sizeof(uint64_t) * missing_asset_count);
//    uint64_t remaining_missing_asset_count = 0;
//    ContentIndex* existing_blocks = GetBlocksForAssets(remote_content_index, missing_asset_count, missing_assets, &remaining_missing_asset_count, remaining_missing_assets);
//    printf("%" PRIu64 " blocks for version `%s` available in content index `%s`\n", *existing_blocks->m_BlockCount, local_path_2, remote_content_path);

//    // Copy existing blocks
//    for (uint64_t block_index = 0; block_index < *missing_content->m_BlockCount; ++block_index)
//    {
//        TLongtail_Hash block_hash = missing_content->m_BlockHash[block_index];
//        char* block_name = GetBlockName(block_hash);
//        char block_file_name[64];
//        sprintf(block_file_name, "%s.lrb", block_name);
//        char* source_path = storage_api.ConcatPath(remote_content_path, block_file_name);
//        StorageAPI_HOpenFile s = storage_api.OpenReadFile(source_path);
//        char* target_path = storage_api.ConcatPath(local_content_path, block_file_name);
//        StorageAPI_HOpenFile t = storage_api.OpenWriteFile(target_path, 0);
//        uint64_t size = storage_api.GetSize(s);
//        char* buffer = (char*)malloc(size);
//        storage_api.Read(s, 0, size, buffer);
//        storage_api.Write(t, 0, size, buffer);
//        Longtail_Free(buffer);
//        storage_api.CloseFile(t);
//        storage_api.CloseFile(s);
//    }

    ContentIndex* merged_local_content = MergeContentIndex(local_content_index, missing_content);
    ASSERT_NE((ContentIndex*)0, merged_local_content);
    Longtail_Free(missing_content);
    missing_content = 0;
    Longtail_Free(local_content_index);
    local_content_index = 0;

    printf("Reconstructing %u assets to `%s`\n", *version2->m_AssetCount, remote_path_2);
    ASSERT_EQ(1, WriteVersion(
        storage_api,
        storage_api,
        compression_registry,
        job_api,
        0,
        0,
        merged_local_content,
        version2,
        local_content_path,
        remote_path_2));
    printf("Reconstructed %u assets to `%s`\n", *version2->m_AssetCount, remote_path_2);

//    Longtail_Free(existing_blocks);
//    existing_blocks = 0;
//    Longtail_Free(remaining_missing_assets);
//    remaining_missing_assets = 0;
//    Longtail_Free(missing_assets);
//    missing_assets = 0;
//    Longtail_Free(remote_content_index);
//    remote_content_index = 0;

    Longtail_Free(merged_local_content);
    merged_local_content = 0;

    Longtail_Free(version1);
    version1 = 0;

    DestroyJobAPI(job_api);
    DestroyHashAPI(hash_api);
    DestroyCompressionRegistry(compression_registry);
    DestroyStorageAPI(storage_api);

    return;
}

TEST(Longtail, Bench)
{
    Bench();
}

TEST(Longtail, LifelikeTest)
{
    LifelikeTest();
}

const uint64_t ChunkSizeAvgDefault    = 64 * 1024;
const uint64_t ChunkSizeMinDefault    = ChunkSizeAvgDefault / 4;
const uint64_t ChunkSizeMaxDefault    = ChunkSizeAvgDefault * 4;

TEST(Longtail, ChunkerLargeFile)
{
    FILE* large_file = fopen("testdata/chunker.input", "rb");
    ASSERT_NE((FILE*)0, large_file);

    fseek(large_file, 0, SEEK_END);
    long size = ftell(large_file);
    fseek(large_file, 0, SEEK_SET);

    struct FeederContext
    {
        FILE* f;
        long size;
        long offset;

        static uint32_t FeederFunc(void* context, struct Chunker* chunker, uint32_t requested_size, char* buffer)
        {
            FeederContext* c = (FeederContext*)context;
            uint32_t read_count = c->size - c->offset;
            if (read_count > 0)
            {
                if (requested_size < read_count)
                {
                    read_count = requested_size;
                }
                int err = fseek(c->f, c->offset, SEEK_SET);
                if (err)
                {
                    return 0;
                }
                uint8_t* p = (uint8_t*)buffer;
                size_t r = fread(buffer, (size_t)read_count, 1, c->f);
                if (r != 1)
                {
                    int e1 = errno;
                    int is_eof = feof(c->f);
                    int e2 = ferror(c->f);
                    return 0;
                }
            }
            c->offset += read_count;
            return read_count;
        }
    };

    FeederContext feeder_context = {large_file, size, 0};

    struct ChunkerParams params = {ChunkSizeMinDefault, ChunkSizeAvgDefault, ChunkSizeMaxDefault};
    Chunker* chunker = CreateChunker(
        &params,
        FeederContext::FeederFunc,
        &feeder_context);

    const uint32_t expected_chunk_count = 20;
    const struct ChunkRange expected_chunks[expected_chunk_count] =
    {
        { (const uint8_t*)0, 0,       81590},
        { (const uint8_t*)0, 81590,   46796},
        { (const uint8_t*)0, 128386,  36543},
        { (const uint8_t*)0, 164929,  83172},
        { (const uint8_t*)0, 248101,  76749},
        { (const uint8_t*)0, 324850,  79550},
        { (const uint8_t*)0, 404400,  41484},
        { (const uint8_t*)0, 445884,  20326},
        { (const uint8_t*)0, 466210,  31652},
        { (const uint8_t*)0, 497862,  19995},
        { (const uint8_t*)0, 517857,  103873},
        { (const uint8_t*)0, 621730,  38087},
        { (const uint8_t*)0, 659817,  38377},
        { (const uint8_t*)0, 698194,  23449},
        { (const uint8_t*)0, 721643,  47321},
        { (const uint8_t*)0, 768964,  86692},
        { (const uint8_t*)0, 855656,  28268},
        { (const uint8_t*)0, 883924,  65465},
        { (const uint8_t*)0, 949389,  33255},
        { (const uint8_t*)0, 982644,  65932}
    };

    ASSERT_NE((Chunker*)0, chunker);

    for (uint32_t i = 0; i < expected_chunk_count; ++i)
    {
        ChunkRange r = NextChunk(chunker);
        ASSERT_EQ(expected_chunks[i].offset, r.offset);
        ASSERT_EQ(expected_chunks[i].len, r.len);
    }
    ChunkRange r = NextChunk(chunker);
    ASSERT_EQ((const uint8_t*)0, r.buf);
    ASSERT_EQ(0, r.len);

    Longtail_Free(chunker);
    chunker = 0;

    fclose(large_file);
    large_file = 0;
}
