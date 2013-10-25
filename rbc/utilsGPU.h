/* This file is part of the Random Ball Cover (RBC) library.
 * (C) Copyright 2010, Lawrence Cayton [lcayton@tuebingen.mpg.de]
 */

#ifndef UTILSGPU_H
#define UTILSGPU_H

#include "defs.h"
//#include<cuda.h>

#include <cstdio>


class OclContextHolder
{
public:
    static cl::Context context;
    static cl::CommandQueue queue;

    static cl::Kernel dist1Kernel;
    static cl::Kernel findRangeKernel;
    static cl::Kernel rangeSearchKernel;
    static cl::Kernel sumKernel;
    static cl::Kernel sumKernelI;
    static cl::Kernel combineSumKernel;
    static cl::Kernel buildMapKernel;
    static cl::Kernel getCountsKernel;
    static cl::Kernel planKNNKernel;
    static cl::Kernel nnKernel;

    static void oclInit();
};


void copyAndMove(ocl_matrix*,const matrix*);
void copyAndMoveI(ocl_intMatrix*,const intMatrix*);
void copyAndMoveC(ocl_charMatrix*,const charMatrix*);

void device_matrix_to_file(const ocl_matrix&, const char*);
void device_matrix_to_file(const ocl_intMatrix&, const char*);

void matrix_to_file(const matrix& mat, const char* filetxt);
void matrix_to_file(const intMatrix& mat, const char* filetxt);


template<typename T>
void generic_write(T val, FILE *fp)
{
}

template<>
inline void generic_write<unint>(unint val, FILE *fp)
{
    fprintf( fp, "%u ", val);
}

template<>
inline void generic_write<real>(real val, FILE *fp)
{
    fprintf( fp, "%f ", val);
}

template<typename T>
inline void array_to_file(T* data, int size, const char* filetxt)
{

    FILE *fp = fopen(filetxt,"w");
    if( !fp ){
      fprintf(stderr, "can't open output file\n");
      return;
    }

    for(int i = 0; i < size; ++i)
    {
        //fprintf( fp, "%f ", data[i]);
        generic_write(data[i], fp);
    }

    fclose(fp);
}

//void copyAndMove(matrix*,const matrix*);
//void copyAndMoveI(intMatrix*,const intMatrix*);
//void copyAndMoveC(charMatrix*,const charMatrix*);

//void checkErr(cudaError_t);
//void checkErr(char*,cudaError_t );

void checkErr(cl_int);
void checkErr(char*, cl_int);

#endif
