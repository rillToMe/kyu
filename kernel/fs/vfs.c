#include "fs.h"

// Fungsi standar pembungkus baca
uint32_t read_fs(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    // Pastikan node punya fungsi read (tidak null pointer)
    if (node->read != 0) {
        return node->read(node, offset, size, buffer);
    }
    return 0; // Kembalikan 0 jika device tidak bisa dibaca
}

// Fungsi standar pembungkus tulis
uint32_t write_fs(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    // Pastikan node punya fungsi write
    if (node->write != 0) {
        return node->write(node, offset, size, buffer);
    }
    return 0;
}

void open_fs(fs_node_t *node) {
    if (node->open != 0) {
        node->open(node);
    }
}

void close_fs(fs_node_t *node) {
    if (node->close != 0) {
        node->close(node);
    }
}