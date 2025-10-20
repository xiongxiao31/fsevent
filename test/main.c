#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <leveldb/c.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <leveldb路径>\n", argv[0]);
        return 1;
    }

    const char *db_path = argv[1];
    const char *prefix = "3:00000009:";
    size_t prefix_len = strlen(prefix);

    char *err = NULL;

    // 打开数据库
    leveldb_options_t *options = leveldb_options_create();
    leveldb_options_set_create_if_missing(options, 0);
    leveldb_t *db = leveldb_open(options, db_path, &err);
    if (err != NULL) {
        fprintf(stderr, "打开数据库失败: %s\n", err);
        leveldb_free(err);
        return 1;
    }

    // 创建读写选项
    leveldb_readoptions_t *ro = leveldb_readoptions_create();
    leveldb_writeoptions_t *wo = leveldb_writeoptions_create();

    // 创建迭代器
    leveldb_iterator_t *it = leveldb_create_iterator(db, ro);

    clock_t start = clock();
    size_t delete_count = 0;

    // 从prefix开头seek
    leveldb_iter_seek(it, prefix, prefix_len);

    // 遍历并删除所有匹配的key
    for (; leveldb_iter_valid(it); leveldb_iter_next(it)) {
        size_t key_len;
        const char *key = leveldb_iter_key(it, &key_len);

        if (key_len < prefix_len || memcmp(key, prefix, prefix_len) != 0) {
            break;  // 超出该前缀范围
        }

        leveldb_delete(db, wo, key, key_len, &err);
        if (err != NULL) {
            fprintf(stderr, "删除key失败: %s\n", err);
            leveldb_free(err);
            err = NULL;
            continue;
        }

        delete_count++;
    }

    clock_t end = clock();
    double elapsed_sec = (double)(end - start) / CLOCKS_PER_SEC;

    printf("共删除 %zu 条以 \"%s\" 开头的数据，耗时 %.3f 秒\n",
           delete_count, prefix, elapsed_sec);

    // 清理资源
    leveldb_iter_destroy(it);
    leveldb_readoptions_destroy(ro);
    leveldb_writeoptions_destroy(wo);
    leveldb_close(db);
    leveldb_options_destroy(options);

    return 0;
}
