#ifndef OaabiHeader
#define OaabiHeader

#ifdef __cplusplus
extern "C" {
#endif

#define OaabiVersion 1

#include <stdint.h>

/**
 * struct OaabiPlugin - 插件入口，布局在 OAABI 1 冻结。
 * @Version: 必须是 OaabiVersion。
 * @Reserved: 必须是 0。
 * @Init: 装入插件时调用。
 * @Fini: 卸载插件时调用。
 */
struct OaabiPlugin {
	uint32_t Version;
	uint32_t Reserved;
	int (*Init)(void);
	void (*Fini)(void);
};

/**
 * OaabiGetVersion() - 返回本头文件的 OAABI 主版本。
 *
 * Return: 恒为 1。
 */
static inline int OaabiGetVersion(void)
{
	return OaabiVersion;
}

#ifdef __cplusplus
}
#endif

#endif
