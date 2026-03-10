#ifndef __WRITEFILE_H__
#define __WRITEFILE_H__

/// <summary>
/// 创建任务队列
/// </summary>
void CreateQueue();

/// <summary>
/// 释放任务队列
/// </summary>
void DeleteQueue();

/// <summary>
/// 创建日志文件，并保留句柄
/// </summary>
void CreateFile();

/// <summary>
/// 释放文件句柄
/// </summary>
void DeleteFile();

/// <summary>
/// 创建日志处理线程
/// </summary>
void CreateThread();

/// <summary>
/// 释放线程
/// </summary>
void DeleteThread();

/// <summary>
/// 启用日志处理线程
/// </summary>
void RunThread();

/// <summary>
/// 将数据写入文件
/// </summary>
void WriteLog();

/// <summary>
/// 获取当前时间，按格式返回字符串
/// </summary>
/// <returns>当前时间 字符串</returns>
string GetCurrentTime();

/// <summary>
/// 将文件名剪裁出来
/// </summary>
/// <returns>文件名</returns>
string GetFileName(string filepath);

/// <summary>
/// 初始化整个日志打印的变量、句柄、集合
/// </summary>
class LxLogWriteFile {
public:
    LxLogWriteFile();
    ~LxLogWriteFile();
};
//void InitDebug();
//void UInitDebug();
#endif // !__WRITEFILE_H__
