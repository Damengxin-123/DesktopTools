#ifndef __VALUE_H__
#define __VALUE_H__
#include <string>
using namespace std;

class LxGeneralType
{
public:
    // 重载所有已知类型，将其转换为字符串
    LxGeneralType();
    LxGeneralType(const char log);
    LxGeneralType(const unsigned long long int da);
    LxGeneralType(const unsigned char log);
    LxGeneralType(const bool log);
    LxGeneralType(const short log);
    LxGeneralType(const unsigned short log);
    LxGeneralType(const unsigned int log);
    LxGeneralType(const int log);
    LxGeneralType(const char* log);
    LxGeneralType(const double log);
    LxGeneralType(const int len, const double log);
    LxGeneralType(const unsigned long log); 
    LxGeneralType(const long log);   
    LxGeneralType(const long long log);
    LxGeneralType(const float log);
    LxGeneralType(const string log);
    LxGeneralType& operator<<(const LxGeneralType data);
    LxGeneralType operator+(LxGeneralType data);

    // 将字符串转换为其他类型
    double ToDouble();
    int ToInt();

    const string GetValue();
    const char* GetData();
    char* GetChar();
    long size();
    // 清理数据
    void Clear();
private:
    string data;
};

#endif // !__VALUE_H__



