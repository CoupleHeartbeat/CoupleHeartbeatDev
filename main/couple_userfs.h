// main/couple_userfs.h —— 用户数据分区（FAT + 磨损均衡）挂载
//
// 分区表中的 "user" 分区位于应用分区之后，刷写合并镜像不清空；
// 挂载为 VFS /userdata，供后续功能存文件（首次挂载失败会自动格式化）。
// 挂载失败不阻塞启动：核心配置在 NVS，不依赖本分区。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COUPLE_USERFS_MOUNT "/userdata"

/**
 * @brief 挂载用户数据分区（幂等）。失败返回错误码且设备继续正常运行。
 */
esp_err_t couple_userfs_init(void);

/**
 * @brief 分区是否已挂载成功（写文件前检查）。
 */
bool couple_userfs_mounted(void);

#ifdef __cplusplus
}
#endif
