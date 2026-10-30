#include "services/FileSearchIndex.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDebug>
#include <windows.h>
#include <psapi.h>
// 使用固定数据和相同查询比较优化前后的检索与分页耗时。
int main(int argc, char** argv) {
 QCoreApplication app(argc,argv);
 qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& message) { const auto bytes=message.toUtf8(); fprintf(stdout,"%s\n",bytes.constData()); fflush(stdout); });
 const int count=argc>1?QByteArray(argv[1]).toInt():1000000;
 FileSearchIndex index; index.upsert(1,0,QStringLiteral("C:"),true);
 QElapsedTimer timer; timer.start();
 for(int i=0;i<count;++i) index.upsert(i+2,1,QStringLiteral("项目资源_%1_%2.dat").arg(i).arg(i%31==0?"texture":"mesh"),false);
 qInfo()<<"build_ms"<<timer.elapsed();
 PROCESS_MEMORY_COUNTERS info{}; GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info));
 qInfo()<<"working_set_mb"<<info.WorkingSetSize/1048576;
 for(const QString& query:{QStringLiteral("texture 987"),QStringLiteral("mesh"),QStringLiteral("ext:dat")}) {
  int total=0,collected=0;timer.restart();
  auto first=index.search(FileSearchIndex::Query::parse(query),0,200,&total,&collected);
  qInfo()<<query<<"first_ms"<<timer.elapsed()<<"total"<<total;
  timer.restart(); auto second=index.search(FileSearchIndex::Query::parse(query),200,200,&total,&collected);
  qInfo()<<query<<"page2_ms"<<timer.elapsed();
 }
}
