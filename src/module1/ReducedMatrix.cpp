#include "ReducedMatrix.h"

ReducedMatrix::ReducedMatrix(double **probMatrix, size_t reducedAlphabetSize){
    
    // swap the matrix and alphabet mappings
    this->origAlphabetSize = this->alphabetSize;
    this->orig_aa2int = this->aa2int;
    this->orig_int2aa = this->int2aa;
    this->origSubMatrix = this->subMatrix;

    // initialize new matrices and alphabet mappings
    this->alphabetSize = reducedAlphabetSize;
    this->aa2int = new int['Z'+1];
    this->int2aa = new char[origAlphabetSize];
    this->reducedAlphabet = new std::vector<char>();
    for (size_t i = 0; i <= 'Z'; ++i) aa2int[i] = orig_aa2int[i];
    for (size_t i = 0; i < origAlphabetSize; ++i){
        int2aa[i] = orig_int2aa[i];
        reducedAlphabet->push_back(int2aa[i]);
    }
    
    double ** subMatrix_tmp=new double*[origAlphabetSize-1];
    double ** probMatrix_new=new double*[origAlphabetSize-1];
    for(size_t i = 0; i<origAlphabetSize-1;i++){
        subMatrix_tmp[i]=new double[origAlphabetSize-1];
        probMatrix_new[i]=new double[origAlphabetSize-1];
    }

    generateSubMatrix(probMatrix, subMatrix_tmp, origAlphabetSize-1);

    double info = calculateMutualInformation(probMatrix, subMatrix_tmp, origAlphabetSize-1);
//    std::cout << "20 " << info << "\n";

    
    size_t reduce_steps=origAlphabetSize-alphabetSize;
    
    
    for(size_t step = 0 ; step < reduce_steps; step++){
        // Ensuring every element is 0.
        for(size_t j = 0; j < this->origAlphabetSize-1; j++)
        {
            for(size_t i = 0; i < this->origAlphabetSize-1; i++)
            {
                probMatrix_new[i][j] = 0;
            }
        }
        //This is where the function to couple the two bases is called.
        std::pair<size_t,size_t> reduce_bases=coupleWithBestInfo(probMatrix, probMatrix_new, origAlphabetSize-1-step);
        
        size_t reduced_index=reduce_bases.first;
        size_t lost_index=reduce_bases.second;
        
        char reduced_aa=reducedAlphabet->at(reduced_index);
        char lost_aa   =reducedAlphabet->at(lost_index);
        
        //printf("%c -> %c\n",lost_aa, reduced_aa);
        reducedAlphabet->erase(reducedAlphabet->begin()+lost_index);
        
        size_t reduced_int=this->orig_aa2int[reduced_aa];
        size_t lost_int   =this->aa2int[lost_aa];

        for(size_t i =0; i < this->origAlphabetSize;i++){
            if(this->int2aa[i]==lost_aa){
                this->int2aa[i]=reduced_aa;
            }
        }
        for(size_t i =0; i < 'Z'; i++){
            if(this->aa2int[i]==lost_int){
                this->aa2int[i]=(int)reduced_int;
            }
        }

       copyMatrix(probMatrix_new,probMatrix, origAlphabetSize-1);

    }
    
    // map big index to new small index
    for(size_t i = 0; i<reducedAlphabet->size();i++){
        const char reduced_aa = reducedAlphabet->at(i);
        int big_int= this->orig_aa2int[reduced_aa];
        for(size_t j =0; j < 'Z'; j++){
            if(this->aa2int[i] == big_int){
                this->aa2int[j] = i;
            }
        }
    }

    // add amino acid X
    int2aa[alphabetSize - 1] = 'X';
    aa2int['X'] = alphabetSize - 1;

    this->subMatrix = new short*[alphabetSize];
    for (int i = 0; i<alphabetSize; i++)
        this->subMatrix[i] = new short[alphabetSize];

    generateSubMatrix(probMatrix_new, this->subMatrix, alphabetSize, 2.0, 0.0);

    for (size_t i = 0; i < origAlphabetSize-1; i++)
    {
        delete[]probMatrix_new[i];
    }
    delete[]probMatrix_new;
}


ReducedMatrix::~ReducedMatrix(){
    delete[] this->orig_aa2int;
    delete[] this->orig_int2aa;
    for(size_t i = 0; i<alphabetSize;i++){
        delete[] origSubMatrix[i];
    }
    delete[] origSubMatrix;

}



void ReducedMatrix::copyMatrix(double ** input,double ** output, size_t size){
    
    for (size_t i=0; i< size; i++){
        for (size_t j=0; j< size; j++){
            output[i][j] = input[i][j];
        }
    }
}


double ReducedMatrix::calculateMutualInformation(double ** pMatrix, double ** subMatrix, size_t size){
    double mutualInfo = 0;
    
    for (size_t i=0; i< size; i++){
        for (size_t j=0; j< size; j++){
            mutualInfo += pMatrix[i][j]*subMatrix[i][j];
        }
    }
    
    // This is to incorporate the factor of 3 in the formula and also the fact
    // that the pMatrix being used by us is in 10E-6.
    
    return mutualInfo;
}



void ReducedMatrix::coupleBases(double ** input, double ** output, size_t size, size_t base1, size_t base2){
    
    double ** temp=new double *[this->origAlphabetSize-1];
    //To ensure every element of temp is set to 0.
    for(size_t i = 0; i < this->origAlphabetSize-1; i++)
    {
        temp[i]=new double[this->origAlphabetSize-1];
        
        for(size_t j = 0; j < this->origAlphabetSize-1; j++)
        {
            temp[i][j] = 0;
        }
    }
    
    //Add corresponding columns first.
    addTwoColumns(input, temp, size, base1, base2);
    
    //Add the corresponding rows.
    addTwoRows(temp, output, size, base1, base2);
    
    for (size_t i = 0; i < this->origAlphabetSize-1; i++)
    {
        delete[]temp[i];
    }
    delete[]temp;

}



std::pair<size_t,size_t> ReducedMatrix::coupleWithBestInfo(double ** pinput, double ** pMatrix, size_t size){
    double bestInfo = 0;
    size_t besti = 0, bestj = 0;
    
    
    // To store the temporary substitution matrix after each coupling.
    double ** tempsub=new double *[size];
    // To store the temporary probability matrix after each coupling.
    double ** tempp=new double *[size];
    
    for(size_t i = 0; i < size; i++){
        tempsub[i]=new double [size];
        tempp[i]=new double [size];
        
    }
    
    for (size_t i=0; i< size; i++){
        
        // To store the mutual information of the matrix.
        double temp = 0;
        
        for (size_t j=i+1; j< size; j++){
            coupleBases(pinput, tempp, size, i, j);
            
            // Generate the new substitution matrix after two bases have been coupled.
            generateSubMatrix(tempp, tempsub, size-1);
            
            // Storing mutual information in temp.
            temp = calculateMutualInformation(tempp, tempsub, size-1);
            
            if (temp > bestInfo) {bestInfo = temp; besti = i; bestj = j;}
            
//            std::cout << " i = " << i << "; j = " << j << " info " << temp << '\n';
        }
        
    }
    
    
    //std::cout << (size-1) <<  " " << bestInfo << "\n";
    // Finally coupling the best option.
    coupleBases(pinput, pMatrix, size, besti, bestj);
    for (size_t i = 0; i < size; i++)
    {
        delete[]tempsub[i];
        delete[]tempp[i];
    }
    delete[]tempsub;
    delete[]tempp;
    return std::make_pair<size_t,size_t>(besti,bestj);
}

void ReducedMatrix::addTwoColumns(double ** input, double ** output, size_t size, size_t col1, size_t col2 ){
    
    for(size_t i = 0; i < size; i++){
        //copy the same data until col2 (excluding)
        for(size_t j = 0; j < col2; j++) {
            output[i][j] = input[i][j];
        }
        
        //Add col2 to col1.
        output[i][col1] = input[i][col1] + input[i][col2];
        
        //shift the rest of the columns left by 1 place.
        for(size_t j = col2 ; j < size-1; j++) {
            output[i][j] = input[i][j+1];
        }

        // set the last column to 0.0
        for (size_t i = 0; i < size; i++)
            output[i][size-1] = 0.0;
    }
}



void ReducedMatrix::addTwoRows(double ** input, double ** output, size_t size, size_t row1, size_t row2 )
{
    
    //copy the same data until row2 (excluding)
    for(size_t i = 0; i < row2; i++){
        for(size_t j = 0; j < size; j++)
            output[i][j] = input[i][j];
    }
    
    //add row2 to row1
    for(size_t j = 0; j < size; j++){
        output[row1][j] = input[row1][j] + input[row2][j];
    }
    
    //shift the rest of rows above by 1 place
    for(size_t i = row2; i < size-1; i++){
        for(size_t j = 0; j < size; j++)
            output[i][j] = input[i+1][j];
    }

    for (size_t j = 0; j < size; j++)
        output[size-1][j] = 0.0;
    
}