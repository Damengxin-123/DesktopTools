#ifndef _TWODIMARRAY_HPP_
#define _TWODIMARRAY_HPP_
#include <QVector>
#include <string>

// 二维int 型数组模板  容器版 用于增删操作
template <typename D>
class TwoDimVector
{
public:
	TwoDimVector() {}
	// 构造函数
	TwoDimVector(int SizeX, int SizeY) {
		if (SizeX < 1 || SizeY < 1) {
			return;
		}
		for (int i = 0; i < SizeX; i++) {
			m_OneDim.append(QVector<D>(SizeY));
		}
	}
	// 新增列
	 void AddVector(QVector<D>& data) {
	 	m_OneDim.append(data);
	 }
	// 通过下标获取
	QVector<D>& operator[](int index) {
		if (index >= m_OneDim.size()) {
			return m_OneDim[m_OneDim.size() - 1];
		}
		return m_OneDim[index];
	}
	// 删除列
	void DelPointDataX(int x) {
		m_OneDim.remove(x);
	}
	//  输入坐标是否在本矩阵中
	bool IsContain(int x, int y) {
		return !(x >= XSize() || y >= YSize() || x < 0 || y < 0);
	}
	// 释放数组
	~TwoDimVector() {
		this->m_OneDim.clear();
	}
	// 列数
	const int XSize() {
		return m_OneDim.size();
	}
	// 行数
	const int YSize() {
		if (XSize() > 0) {
			return m_OneDim[0].size();
		} else {
			return 0;
		}
	}
	// 行反转
	void YReversal() {
		D dataTmp;
		int size = 0;
		for (int i = 0; i < m_OneDim.size(); i++) {
			size = m_OneDim[i].size();
			for (int j = 0; j < size / 2; j++) {
				dataTmp = m_OneDim[i][j];
				m_OneDim[i][j] = m_OneDim[i][size - 1 - j];
				m_OneDim[i][size - 1 - j] = dataTmp;
			}
		}
	}
	// 初始化数组
	void InitData(D data) {
		for (int i = 0; i < XSize(); i++) {
			for (int j = 0; j < YSize(); j++) {
				m_OneDim[i][j] = data;
			}
		}
	}
	// 从本矩阵中剪裁一部分出来
	void GetPartData(int x1, int y1, int x2, int y2, TwoDimVector<D>& out_data) {
		bool ret = IsContain(x1, y1);
		ret = ret && IsContain(x2, y2);

		if (!ret) {
			return ;
		}

		out_data = TwoDimVector<D>(abs(x2 - x1), abs(y2 - y1));

		int minX = x2 > x1 ? x1 : x2;
		int minY = y2 > y1 ? y1 : y2;

		for (int i = 0; i < abs(x2 - x1); i++) {
			for (int j = 0; j < abs(y2 - y1); j++) {
				out_data[i][j] = m_OneDim[minX + i][minY + j];
			}
		}
	}
	// 释放全部数据 清理内存
	void ClearData() {
		m_OneDim.clear();
	}
public:
	// 初始化时临时矩阵 用于降低数据精度
	QVector<QVector<D>> m_OneDim;

};

// 二维数组模板 用于快速查改 **************************************************************************************
// 本数组按列依次存取
template <typename D>
class TwoDimArray
{
public:
	TwoDimArray() {}
	// 构造函数
	TwoDimArray(TwoDimArray<D>& ta) {
		// 线释放本数据结构的数据
		if (m_DataArr) {
			delete[] m_DataArr;
			m_Xsize = 0;
			m_Ysize = 0;
			m_DataArr = nullptr;
		}
		m_Xsize = ta.XSize();
		m_Ysize = ta.YSize();


		// 拷贝数据
		m_DataArr = new D[m_Xsize * m_Ysize];


		memset(m_DataArr, 0, m_Xsize * m_Ysize * sizeof(D));

		memcpy(m_DataArr, ta.m_DataArr, m_Xsize * m_Ysize *sizeof(D));


		//for (int i = 0; i < ta.XSize(); i++) {
		//	for (int j = 0; j < ta.YSize(); j++) {
		//		this->operator[](i)[j] = ta[i][j];
		//	}
		//}
	}


	TwoDimArray(int SizeX, int SizeY) {
		if (SizeX < 1 || SizeY < 1) {
			return;
		}
		m_DataArr = new D[SizeX * SizeY];
		memset(m_DataArr, 0, SizeX * SizeY * sizeof(D));
		m_Xsize = SizeX;
		m_Ysize = SizeY;
	}
	void InitArr(int SizeX, int SizeY) {
		if (m_DataArr || SizeX < 1 || SizeY < 1) {
			return;
		}
		m_DataArr = new D[SizeX * SizeY];
		memset(m_DataArr, 0, SizeX * SizeY * sizeof(D));
		m_Xsize = SizeX;
		m_Ysize = SizeY;
	}
	// 释放数组
	~TwoDimArray() {
		if (m_DataArr) {
			delete[] m_DataArr;
			m_Xsize = 0;
			m_Ysize = 0;
			m_DataArr = nullptr;
		}
	}
	// 通过下标获取   
	D* operator[](int Xindex) {
		if (Xindex < m_Xsize && Xindex >= 0) {
			return m_DataArr + (Xindex * m_Ysize);
		}
		return nullptr;
	}
	// 赋值符号赋值
	void operator=(TwoDimArray<D>& ta) {
		// 线释放本数据结构的数据
		if (m_DataArr) {
			delete[] m_DataArr;
			m_Xsize = 0;
			m_Ysize = 0;
			m_DataArr = nullptr;
		}	

		m_Xsize = ta.XSize();
		m_Ysize = ta.YSize();
		// 拷贝数据
		m_DataArr = new D[m_Xsize * m_Ysize];


        memset(m_DataArr, 0, m_Xsize * m_Ysize * sizeof(D));

		memcpy(m_DataArr, ta.m_DataArr, m_Xsize * m_Ysize *sizeof(D));


		//for (int i = 0; i < ta.XSize(); i++) {
		//	for (int j = 0; j < ta.YSize(); j++) {
		//		this->operator[](i)[j] = ta[i][j];
		//	}
		//}
		
	}
	//  输入坐标是否在本矩阵中
	bool IsContain(int x, int y) {
		return !(x >= XSize() || y >= YSize() || x < 0 || y < 0);
	}
	
	// 列数
	const int XSize() {
		return m_Xsize;
	}
	// 行数
	const int YSize() {
		return m_Ysize;
	}
	// 初始化数组
	void InitData(D data) {
		memset(m_DataArr, data, m_Xsize * m_Ysize * sizeof(D));

		//for (int i = 0; i < m_Xsize * m_Ysize; i++) {
		//	m_DataArr[i] = data;
		//}
	}
	// 从本矩阵中剪裁一部分出来
	void GetPartData(int x1, int y1, int x2, int y2, TwoDimArray<D>& out_data) {
		bool ret = IsContain(x1, y1);
		ret = ret && IsContain(x2, y2);

		if (!ret) {
			return;
		}

		out_data = TwoDimArray<D>(abs(x2 - x1), abs(y2 - y1));

		int minX = x2 > x1 ? x1 : x2;
		int minY = y2 > y1 ? y1 : y2;

		for (int i = 0; i < abs(x2 - x1); i++) {
			for (int j = 0; j < abs(y2 - y1); j++) {
				out_data[i][j] = this->operator[](minX + i)[minY + j];
			}
		}
	}
	QString name;
private:

	long m_Xsize{ 0 };
	long m_Ysize{ 0 };
	// 精度降低后的数据，用于提高读写速度
	D* m_DataArr{ nullptr };
};











#endif // !_TWODIMARRAY_HPP_
