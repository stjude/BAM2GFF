#ifndef LIQUIDATOR_BAM_SCORER_H_INCLUDED
#define LIQUIDATOR_BAM_SCORER_H_INCLUDED

#include "bamliquidator_regions.h"
#include "score_matrix.h"

#include <samtools/bam.h>

#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace liquidator
{

inline bool unmapped(const bam1_t& read)
{
    // The 3rd bit being set in the flag means it is unmapped.
    // Binary with 3rd bit set (0b100) is 4.
    static const uint32_t unmapped_bit = 4;
    return read.core.flag & unmapped_bit;
}

class BamScorer
{
public:
    BamScorer(const std::string& bam_input_file_path,
              const std::vector<ScoreMatrix>& matrices,
              bool verbose,
              bool only_score_unmapped,
              const std::string& bam_output_file_path,
              const std::string& region_file_path = "")
    :
        m_input(bam_open(bam_input_file_path.c_str(), "r")),
        m_output(0),
        m_header(bam_header_read(m_input)),
        m_index(bam_index_load(bam_input_file_path.c_str())),
        m_matrices(matrices),
        m_verbose(verbose),
        m_only_score_unmapped(only_score_unmapped),
        m_read(0),
        m_read_count(0),
        m_unmapped_count(0),
        m_read_hit_count(0),
        m_unmapped_hit_count(0),
        m_total_hit_count(0)
    {
        if (m_input == 0 || m_header == 0 || m_index == 0)
        {
            throw std::runtime_error("failed to open " + bam_input_file_path);
        }

        if (!bam_output_file_path.empty())
        {
            m_output = bam_open(bam_output_file_path.c_str(), "w");
            bam_header_write(m_output, m_header);
        }

        if (m_verbose)
        {
            std::cout << "#pattern name\tsequence name\tstart\tstop\tstrand\tscore\tp-value\tq-value\tmatched sequence" << std::endl;
        }

        if (!region_file_path.empty())
        {
            score_regions(region_file_path);
        }
        else
        {
            score_all_reads();
        }
    }

    ~BamScorer()
    {
        auto print_percent = [](const std::string& upper_label, size_t upper_value, const std::string& lower_label, size_t lower_value) {
            std::cout << "# (" << upper_label << ") / (" << lower_label << ") = " << upper_value << '/' << lower_value << " = " << 100*(double(upper_value)/lower_value) << '%' << std::endl;
        };

        if (!m_only_score_unmapped)
        {
            print_percent("reads hit", m_read_hit_count, "total reads", m_read_count);
            print_percent("mapped hit", m_read_hit_count - m_unmapped_hit_count, "mapped reads", m_read_count - m_unmapped_count);
        }
        print_percent("unmapped hit", m_unmapped_hit_count, "unmapped reads", m_unmapped_count);
        if (!m_only_score_unmapped)
        {
            print_percent("unmapped hit", m_unmapped_hit_count, "total hit", m_read_hit_count);
        }
        print_percent("unmapped reads", m_unmapped_count, "total reads", m_read_count);
        std::cout << "# total hits: " << m_total_hit_count << " (average hits per hit read = " << double(m_total_hit_count)/m_read_hit_count << ")" << std::endl;

        bam_index_destroy(m_index);
        bam_header_destroy(m_header);
        bam_close(m_input);

        if (m_output)
        {
            bam_close(m_output);
        }
    }

    void operator()(const std::string& motif_name,
                    size_t start,
                    size_t stop,
                    const ScoreMatrix::Score& score)
    {
        if (score.pvalue() < 0.0001)
        {
            ++m_total_hit_count;
            if (m_verbose)
            {
                const char* chromosome = m_read->core.tid >= 0 ? m_header->target_name[m_read->core.tid] : "*";
                std::cout << motif_name << '\t'
                          << (unmapped(*m_read) ? "un" : "") << "mapped:" << chromosome << ":" << (char*) m_read->data << '\t'
                          << m_read->core.pos + start << '\t'
                          << m_read->core.pos + stop << '\t'
                          << (score.is_reverse_complement() ? '-' : '+') << '\t';

                std::cout.precision(6);
                std::cout << score.score() << '\t';
                std::cout.precision(3);

                std::cout << score.pvalue() << '\t'
                          << '\t' // omit q-value for now
                          << score << std::endl;
            }
        }
    }

private:
    void score_all_reads()
    {
        // todo: the unmapped reads seem to all be at the very end of the loop.
        //       to speed up scoring just the unmapped reads, we could probably skip to the last indexed read and start there.
        //       although, that might be relying on undocumented behavior that could change in future releases, so maybe that is a bad idea.
        //       also, there seems to be some mechanism for storing unmapped reads that correspond to a chromosome, so that is probably a doubly bad idea.
        //       see https://www.biostars.org/p/86405/#86439

        auto destroyer = [](bam1_t* p) { bam_destroy1(p); };
        std::unique_ptr<bam1_t, decltype(destroyer)> raii_read(bam_init1(), destroyer);
        bam1_t* read = raii_read.get();
        while (bam_read1(m_input, read) >= 0)
        {
            score_read(read);
        }
    }

    void score_regions(const std::string& region_file_path)
    {
        for (const Region& region : parse_regions(region_file_path, "bed", 0))
        {
            // todo: don't I just need to parse_region once per chromosome to get the tid? perhaps there is a faster way to do this without parsing a whole region string?
            // todo: consider adding a util function to do this and remove duplicate code in bamliquidator.cpp
            std::stringstream coord;
            coord << region.chromosome << ':' << region.start << '-' << region.stop;

            int ref,beg,end;
            const int region_parse_rc = bam_parse_region(m_header, coord.str().c_str(), &ref, &beg, &end);
            if (region_parse_rc != 0)
            {
                std::stringstream error_msg;
                error_msg << "bam_parse_region failed with return code " << region_parse_rc;
                throw std::runtime_error(error_msg.str());
            }
            if(ref<0)
            {
                // this bam doesn't have this chromosome
                continue;
            }

            const int fetch_rc = bam_fetch(m_input, m_index, ref, beg, end, this, bam_fetch_func);
            if (fetch_rc != 0)
            {
                std::stringstream error_msg;
                error_msg << "bam_fetch failed with return code " << fetch_rc;
                throw std::runtime_error(error_msg.str());
            }
        }
    }

    void score_read(const bam1_t* read)
    {
        ++m_read_count;
        if (unmapped(*read))
        {
            ++m_unmapped_count;
        }
        else if (m_only_score_unmapped)
        {
            return;
        }

        const bam1_core_t *c = &read->core;
        uint8_t *s = bam1_seq(read);

        // [s, s+c->l_qseq) is the sequence, with two bases packed into each byte.
        // I bet we could directly search that instead of first copying into a string
        // but lets get something simple working first. An intermediate step could be
        // to search integers without using bam_nt16_rev_table (and I wouldn't have
        // to worry about the packing complexity).

        if (m_sequence.size() != size_t(c->l_qseq))
        {
            // assuming that all reads are uniform length, this will only happen once
            m_sequence = std::string(c->l_qseq, ' ');
        }
        for (int i = 0; i < c->l_qseq; ++i)
        {
            m_sequence[i] = bam_nt16_rev_table[bam1_seqi(s, i)];
        }

        const size_t hit_count_before_this_read = m_total_hit_count;
        m_read = read;
        for (const auto& matrix : m_matrices)
        {
            matrix.score(m_sequence, *this);
        }
        if (m_total_hit_count > hit_count_before_this_read)
        {
            ++m_read_hit_count;
            if (unmapped(*read))
            {
                ++m_unmapped_hit_count;
            }
            if (m_output)
            {
                bam_write1(m_output, read);
            }
        }
    }

    static int bam_fetch_func(const bam1_t* read, void* handle)
    {
        BamScorer& scorer = *static_cast<BamScorer*>(handle);
        scorer.score_read(read);
        return 0;
    }

private:
    bamFile m_input;
    bamFile m_output;
    bam_header_t* m_header;
    bam_index_t* m_index;
    const std::vector<ScoreMatrix>& m_matrices;
    const bool m_verbose;
    const bool m_only_score_unmapped;
    const bam1_t* m_read;
    size_t m_read_count;
    size_t m_unmapped_count;
    size_t m_read_hit_count;
    size_t m_unmapped_hit_count;
    size_t m_total_hit_count;
    std::string m_sequence;
};

}

#endif