#ifndef RANDOM_GENERATOR_H
#define RANDOM_GENERATOR_H

#include <random>
#include <cstdint>

// 全局种子常量
extern const uint32_t GLOBAL_RANDOM_SEED;

// 声明全局随机数生成器（供所有文件使用）
extern std::mt19937 g_randomGenerator;

// 声明全局分布对象
extern std::uniform_real_distribution<double> g_uniformDistribution;

/**
 * \brief 生成一个 [0,1) 范围内的均匀分布随机数
 * \return 随机双精度浮点数
 */
double GenerateRandomDouble();

/**
 * \brief 生成指定范围内的随机整数
 * \param min 最小值（包含）
 * \param max 最大值（包含）
 * \return 随机整数
 */
uint32_t GenerateRandomInt(uint32_t min, uint32_t max);

#endif // RANDOM_GENERATOR_H