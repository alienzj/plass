#include <flash/combine_reads.h>
#include "KSeqWrapper.h"
#include "SubstitutionMatrix.h"
#include "MultipleAlignment.h"
#include "DBReader.h"
#include "DBWriter.h"
#include "Debug.h"
#include "Util.h"
#include "LocalParameters.h"

#ifdef OPENMP
#include <omp.h>
#endif

int mergereads(int argn, const char **argv, const Command& command) {
    LocalParameters& par = LocalParameters::getLocalInstance();
    par.parseParameters(argn, argv, command, 2, true, Parameters::PARSE_VARIADIC);

    // + 1 for query
    SubstitutionMatrix subMat(par.scoringMatrixFile.c_str(), 2.0f, 0.0f);
    Debug(Debug::INFO) << "Start merging reads.\n";

    //TODO
    combine_params alg_params;
    alg_params.max_overlap = 65;
    alg_params.min_overlap = 10;
    alg_params.max_mismatch_density = 0.25;
    alg_params.cap_mismatch_quals = false;
    alg_params.allow_outies = false;

    std::vector<std::string> filenames(par.filenames);
    std::string outFile = par.filenames.back();
    std::string outIndexFile = outFile + ".index";
    DBWriter resultWriter(outFile.c_str(), outIndexFile.c_str());
    resultWriter.open();
    DBWriter headerResultWriter((outFile+"_h").c_str(), (outFile+"_h.index").c_str());
    headerResultWriter.open();
    unsigned int id = 0;
    {
        struct read* r1 = (struct read*) calloc(1, sizeof(struct read));
        struct read* r2 = (struct read*) calloc(1, sizeof(struct read));
        struct read* r_combined = (struct read*) calloc(1, sizeof(struct read));

        for (size_t i = 0; i < filenames.size() / 2; i++) {
            std::string splitHeader;
            splitHeader.reserve(1024);
            std::string header;
            header.reserve(1024);
            std::string splitId;
            splitId.reserve(1024);
            char lookupBuffer[32768];
            KSeqWrapper *kseq1 = KSeqFactory(filenames[i * 2].c_str());
            KSeqWrapper *kseq2 = KSeqFactory(filenames[i * 2 + 1].c_str());
            while (kseq1->ReadEntry() && kseq2->ReadEntry()) {
                const KSeqWrapper::KSeqEntry &read1 = kseq1->entry;
                const KSeqWrapper::KSeqEntry &read2 = kseq2->entry;

                if (read1.qual.l != read2.qual.l) {
                    Debug(Debug::ERROR) << "Invalid FASTQ entry " << i << ".\n";
                    return EXIT_FAILURE;
                }

                r1->seq =  read1.sequence.s;
                r1->seq_len = read1.sequence.l;
                r1->qual = read1.qual.s;
                r1->qual_len = read1.qual.l;

                r2->seq = read2.sequence.s;
                r2->seq_len = read2.sequence.l;
                r2->qual = read2.qual.s;
                r2->qual_len = read2.qual.l;
                reverse_complement(r2);
                enum combine_status status = combine_reads(r1, r2, r_combined, &alg_params);
                char newLine = '\n';
                switch (status) {
                    case COMBINED_AS_INNIE:
                    case COMBINED_AS_OUTIE:
                        //resultWriter.writeData(r_combined->seq, r_combined->seq_len,  id);
                        resultWriter.writeStart(0);
                        resultWriter.writeAdd(r_combined->seq, r_combined->seq_len, 0);
                        resultWriter.writeAdd(&newLine, 1, 0);
                        resultWriter.writeEnd(id, 0, true);
                        headerResultWriter.writeData(read1.name.s, read1.name.l, id, 0, true );
                        break;
                    case NOT_COMBINED:
                        resultWriter.writeStart(0);
                        resultWriter.writeAdd(r1->seq, r1->seq_len, 0);
                        resultWriter.writeAdd(&newLine, 1, 0);
                        resultWriter.writeEnd(id, 0, true);
                        headerResultWriter.writeData(read1.name.s, read1.name.l, id, 0, true );
                        id++;
                        resultWriter.writeStart(0);
                        resultWriter.writeAdd(r2->seq, r2->seq_len, 0);
                        resultWriter.writeAdd(&newLine, 1, 0);
                        resultWriter.writeEnd(id, 0, true);
                        headerResultWriter.writeData(read2.name.s, read1.name.l, id, 0, true );
                        break;
                }
                id++;
            }
            delete kseq1;
            delete kseq2;
        }
        free(r_combined);
        free(r2);
        free(r1);
    }
    resultWriter.close(Sequence::NUCLEOTIDES);
    headerResultWriter.close();

    Debug(Debug::INFO) << "\nDone.\n";

    return EXIT_SUCCESS;
}



