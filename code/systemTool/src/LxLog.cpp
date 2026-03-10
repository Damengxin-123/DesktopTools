#pragma warning(disable:4996)
#include <queue>
#include <thread>
#include <fstream>
#include <map>
#include <time.h>
#include <ctime>
#include <iostream>
#include <iomanip>
#include <stdio.h>
#include <vector>
#include <chrono>
#include "LxLog.h"
#include "LxLogWriteFile.h"

typedef struct {
    int     type;
    string  file;
    string  func;
    long    line;
    string  logs;
    string  time;
}LOG_TASK;
namespace LOG_CONF {
    // 日志输出配置   50 M
    const long long file_max_size = 20 * 1024 * 1024;
    const int file_size = 4;
    const char* file_path = "./logs/";
    const char* dir_name = "logs";
    const char* file_type = ".log"; 
    const char* file_names[file_size] = { "debug", "run", "error", "test"};
};
namespace OUT_LOG {
    queue<LOG_TASK>*  task_queue { nullptr };
    vector<FILE*>     files;
    // map<int, FILE*>*  file_lists { nullptr };
    thread*            write_file { nullptr };;
    static LxLogWriteFile         init_debug;
};

using namespace OUT_LOG;
LxLog::LxLog(int type, string file, string func, long line)
{
    this->m_type = type;
    this->m_file = file;
    this->m_func = func;
    this->m_line = line;
}

LxLog::LxLog(const int type, const char file[], const char func[], const long line)
{
    this->m_type = type;
    this->m_file = file;
    this->m_func = func;
    this->m_line = line;
}

LxLog::LxLog()
{
    this->m_type = 3;
}

LxLog& LxLog::operator<<(LxGeneralType log)
{
    m_logs.append(log.GetValue());
    return *this;
}

LxLog::~LxLog()
{
    if (task_queue) {
        LOG_TASK tmp;
           tmp.type =  m_type;
           tmp.file =  m_file;
           tmp.func =  m_func;
           tmp.line =  m_line;
           tmp.logs =  m_logs;
           tmp.time = GetCurrentTime();
           task_queue->push(tmp);
    }
    RunThread();
    //WriteLog();
    // RunThread();
}
/*---------writefile.h----------*/
using namespace OUT_LOG;
using namespace LOG_CONF;
void CreateQueue()
{
    if (!task_queue) {
        task_queue = new queue<LOG_TASK>();
    }
}

void DeleteQueue()
{
    if (task_queue) {
        while (!task_queue->empty()) {
            task_queue->pop(); 
        }
    }
}

void CreateFile()
{
    if (files.size() >= file_size) {
        return;
    }
    /*if (!file_lists) {
        file_lists = new  map<int, FILE*>();
     }*/
    // 创建文件夹
    LxGeneralType shell = LxGeneralType() << "mkdir " << dir_name;

    system(shell.GetData());

    FILE *fp = nullptr;
    //errno_t errcode;
    // 遍历文件名数组，写入文件名
    LxGeneralType filename;
    for (int i = 0; i < file_size; i++) { 
        filename = LxGeneralType() << file_path << file_names[i] << file_type;
        fp = fopen(filename.GetData(), "a+");
        if (fp) {
            // 检测文件大小 删除过大日志
            fseek(fp, 0, SEEK_END);
            long len = ftell(fp);
            if (len > file_max_size) {
                fclose(fp);
                remove(filename.GetData());
                
                // 然后重新创建文件
                fp = fopen(filename.GetData(), "a+");
                if (fp) {
                    files.push_back(fp);
                }
            } else {
                files.push_back(fp);
            }
            
            // files.insert(map<int, FILE*>::value_type(001, fp));
        } else {
            return;
        }
    }
}

void DeleteFile()
{
    if (files.size()) {
        FILE* fp = nullptr;
        // 遍历文件名数组，写入文件名
        for (int i = 0; i < files.size(); i++) {
            fp = files[i];
            if (fp) {
                fclose(fp);
            } else {
                return;
            }
        }
        files.clear();
    }
}

void CreateThread()
{
    if (!write_file) {
        write_file = new thread();
   }
}

void DeleteThread()
{
    if (write_file && write_file->joinable()) {
        write_file->join();
    } else if(write_file) {
        delete write_file;
        write_file = nullptr;
    }
}

void RunThread()
{   
    
    if (write_file && write_file->joinable()) {
        return;
    } else if(write_file) {
        delete write_file;
        write_file = new thread(WriteLog);
        write_file->join();
    }
}

void WriteLog()
{
    if (task_queue && !files.empty()) {
        LOG_TASK tmp;
        FILE* fp = nullptr;
        LxGeneralType log;
        while (!task_queue->empty()) {
            tmp = task_queue->front();
            fp = files[tmp.type];
            // 如果为测试数据输出，则不打印日志信息
            if (fp) {
                if (tmp.type >= 3) {
                    log << tmp.logs << "\n";
                } else {
                    log << "[" << tmp.time << "]-[" << GetFileName(tmp.file) << "]-[" << tmp.line << "]-[" << tmp.func << "]:" << tmp.logs << "\n";
                }
                // 写入并刷新文件
                fwrite(log.GetValue().c_str(), log.size(), 1, fp);
                fflush(fp);
            } else {
                cout << "fp is null!";
            } 
            task_queue->pop();
        }
    }
}

using namespace std::chrono;
string GetCurrentTime()
{
    auto now = system_clock::now();

    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    auto posix = system_clock::to_time_t(now);

    char buf[32]; 
    std::tm tp;
    tp = *localtime(&posix);

    std::strftime(buf, sizeof(buf), "%F %T", &tp);

    LxGeneralType ret = LxGeneralType(buf) + ":" + LxGeneralType(ms.count());
    

    return ret.GetValue();
}
string GetFileName(string filepath)
{
    string ret;
    for (int i = filepath.size() - 1; i >= 0; i--) {
        if (filepath[i] == '\\') {
            ret.append(filepath.substr(i + 1));
            return ret;
        }
    }
    return ret;
}
LxLogWriteFile::LxLogWriteFile()
{
    CreateQueue();
    CreateThread();
    CreateFile();
}

LxLogWriteFile::~LxLogWriteFile()
{
    DeleteQueue();
    DeleteThread();
    DeleteFile();
}
