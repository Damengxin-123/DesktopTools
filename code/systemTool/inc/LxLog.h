#ifndef DEBUG_H
#define DEBUG_H
#include <string>
#include "LxGeneralType.h"
using namespace std;

/************************C++风格**********************************/
// 调试信息输出
#define DEBUG() LxLog(0, __FILE__, __func__, __LINE__)
// 运行信息输出
#define RUNLOG() LxLog(1, __FILE__, __func__, __LINE__)
// 错误信息输出
#define ERRLOG() LxLog(2, __FILE__, __func__, __LINE__)
// 数据打印宏定义 用于大量数据输出到文件
#define TESTLOG() LxLog()

// 对指针判空的宏定义
#define TO_STR(P) #P
#define IS_NULL(P, RET) if(P == nullptr){ ERRLOG() << "ERROR: " << TO_STR(P) << " is nullptr!!"; return RET;}
#define IS_FALSE(F, RET) if(!F){ ERRLOG() << "ERROR: " << TO_STR(F) << " run is error! quit!"; return RET;}


// 释放资源
// 释放普通指针
#define DEL_PTR(P) if(P) { delete P;P = nullptr;}
// 释放一维数组
#define DEL_ARR(P) if(P) { delete[] P;P = nullptr;}
// 释放二维数组
#define DEL_ARR2(P,LEN) if(P) { for(int ixxxxxi = 0; ixxxxxi < LEN; ixxxxxi ++){ DEL_ARR(P[ixxxxxi]); } P = nullptr; }


class LxLog
{
public:         
    /// <summary>
    /// Initializes a new instance of the <see cref="Debug"/> class.
    /// </summary>
    /// <param name="type">日志输出到那个文件, 详见日志输出配置.</param>
    /// <param name="file">日志信息文件名.</param>
    /// <param name="func">日志信息函数名</param>
    /// <param name="line">日志信息行号</param>
    LxLog(int type, string file, string func, long line);
    LxLog(const int type, const char file[], const char func[], const long line);
    /// <summary>
    /// 仅输出字符串，不显示输出着信息
    /// </summary>
    LxLog();
    /// <summary>
    /// 重载 运算 符号，方便使用<see cref="Debug"/> class.
    /// </summary>
    /// <param name="log">日志内容.</param>
    LxLog& operator<<(LxGeneralType log);
    // 析构函数时处理日志数据
    ~LxLog();
private:
    int     m_type;
    string  m_file;
    string  m_func;
    long    m_line;
    string  m_logs;
    string  m_time;
};




#endif // !DEBUG_H