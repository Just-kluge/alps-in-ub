#include "random_generator.h"

// 定义全局种子
const uint32_t GLOBAL_RANDOM_SEED = 42;

// 定义全局随机数生成器（使用固定种子初始化）
std::mt19937 g_randomGenerator(GLOBAL_RANDOM_SEED);

// 定义全局均匀分布对象 [0,1)
std::uniform_real_distribution<double> g_uniformDistribution(0.0, 1.0);

// 实现辅助函数
double GenerateRandomDouble() {
    return g_uniformDistribution(g_randomGenerator);
}

uint32_t GenerateRandomInt(uint32_t min, uint32_t max) {
    std::uniform_int_distribution<uint32_t> dist(min, max);
    return dist(g_randomGenerator);
}