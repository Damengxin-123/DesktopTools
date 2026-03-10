#include "LxGeneralType.h"
#include <string.h>
LxGeneralType::LxGeneralType()
{
    this->data = "";
}

LxGeneralType::LxGeneralType(char log)
{
    this->data = to_string(log);
}

LxGeneralType::LxGeneralType(const unsigned long long int da)
{
    this->data = to_string(da);
}

LxGeneralType::LxGeneralType(unsigned char log)
{
    this->data = to_string(log);
}

LxGeneralType::LxGeneralType(bool log)
{
    log ? this->data = "true" : this->data = "false";
}

LxGeneralType::LxGeneralType(short log)
{
    this->data = to_string(log);
}

LxGeneralType::LxGeneralType(unsigned short log)
{
    this->data = to_string(log);
}

LxGeneralType::LxGeneralType(unsigned int log)
{
    this->data = to_string(log);
}

LxGeneralType::LxGeneralType(int log)
{
    this->data = to_string(log);
}

LxGeneralType::LxGeneralType(const char* log)
{
    this->data = log;
}

LxGeneralType::LxGeneralType(double log)
{
    this->data = to_string(log);
}

LxGeneralType::LxGeneralType(const int len, const double log)
{
    // 记录其小数部分
    int* buff = new int[len + 1];
    // 遍历小数部分 将其存入数组
    double xs = log - (double)((int)log);
    double index = 1;
    for (int i = 1; i <= len; i++) {
        buff[i - 1] = (int)(xs / (index / 10));
        xs -= (index / 10) * (double)((int)(xs / (index / 10)));
        index /= 10;
    }

    for (int i = 1; i <= len; i++) {
        data.append(to_string(buff[i - 1]));
    }
    
}

LxGeneralType::LxGeneralType(unsigned long log)
{
    this->data = to_string(log);
}

LxGeneralType::LxGeneralType(long log)
{
    this->data = to_string(log);
}

LxGeneralType::LxGeneralType(long long log)
{
    this->data = to_string(log);
}

LxGeneralType::LxGeneralType(float log)
{
    this->data = to_string(log);
}

LxGeneralType::LxGeneralType(string log)
{
    this->data = log;
}

LxGeneralType& LxGeneralType::operator<<(LxGeneralType data)
{
    this->data.append(data.GetValue());
    return *this;
}

LxGeneralType LxGeneralType::operator+(LxGeneralType data)
{
    return LxGeneralType(this->GetValue() + data.GetValue());
}

double LxGeneralType::ToDouble()
{
    return stod(this->data);
}

int LxGeneralType::ToInt()
{
    return stoi(this->data);
}

const string LxGeneralType::GetValue()
{
    return this->data;
}
const char* LxGeneralType::GetData()
{
    return this->data.c_str();
}

char* LxGeneralType::GetChar()
{
    size_t si = this->data.size() + 1;
    char* ret = new char[si];
    memset(ret, 0, si);
    memcpy(ret, this->data.c_str(), si - 1);
    return ret;
}

long LxGeneralType::size()
{
    return data.size();
}

void LxGeneralType::Clear()
{
    data = "";
}
