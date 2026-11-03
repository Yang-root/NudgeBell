// VibeFocus 音效引擎 —— 纯 C++ 实时合成 + waveOut 输出
#pragma once
#include <vector>
#include <string>

namespace vf {

constexpr int SR = 44100;
using Buf = std::vector<float>;

int soundCount();
const wchar_t* soundName(int id);
const wchar_t* soundTag(int id);

// 生成一段提示音（id 0..11），返回 -1..1 的单声道浮点样本
Buf makeSound(int id);

// 生成环境音循环片段：1=雨声 2=棕噪音 3=低频
Buf makeAmbient(int kind);

// 播放一次（异步，内部线程）
void playOnce(const Buf& b, float volume);

// 循环播放环境音
void startAmbient(int kind, float volume);
void stopAmbient();
void setAmbientVolume(float volume);

} // namespace vf
