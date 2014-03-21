#include "bamliquidator.h"

#include <cmath>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

#include <hdf5.h>
#include <hdf5_hl.h>

// this CountH5Record must match exactly the structure in HDF5
// -- see bamliquidator_batch.py function create_count_table
struct CountH5Record
{
  char cell_type[16];
  char file_name[64];
  char chromosome[16];
  uint32_t bin_number;
  uint64_t count;
  double normalized_count;
};

std::vector<CountH5Record> count(const std::string chr,
                                 const std::string cell_type,
                                 const unsigned int bin_size,
                                 const size_t length,
                                 const std::string bam_file)
{
  const std::string bam_file_name = file_name_from_path(bam_file);

  int bins = std::ceil(length / (double) bin_size);
  int max_base_pair = bins * bin_size;

  const std::vector<double> bin_counts = liquidate(bam_file, chr, 0, max_base_pair, '.', bins, 0);

  CountH5Record record;
  strncpy(record.cell_type,  cell_type.c_str(),     sizeof(CountH5Record::cell_type));
  strncpy(record.file_name,  bam_file_name.c_str(), sizeof(CountH5Record::file_name));
  strncpy(record.chromosome, chr.c_str(),           sizeof(CountH5Record::chromosome));
  record.bin_number = 0;
  record.count = 0;
  record.normalized_count = 0;

  /* Excerpt from Feb 13, 2014 email from Charles Lin:

  We typically report read density in units of reads per million per basepair

  bamliquidator reports counts back in total read positions per bin.  To convert that 
  into reads per million per basepair, we first need to divide by the total million 
  number of reads in the bam.  Then we need to divide by the size of the bin

  So for instance if you have a 1kb bin and get 2500 counts from a bam with 30 million
  reads you would calculate density as 2500/1000/30 = 0.083rpm/bp */

  const double normalization_factor =
    (1 / (double) bin_size) * (1 / (length / (double) std::pow(10,6)));

  std::vector<CountH5Record> records(bin_counts.size(), record);
  for (size_t bin=0; bin < bin_counts.size(); ++bin)
  {
    records[bin].bin_number = bin;
    records[bin].count = bin_counts[bin];
    records[bin].normalized_count = bin_counts[bin] * normalization_factor;
  }

  return records;
}

void batch(hid_t& file,
           const std::string& cell_type,
           const unsigned int bin_size,
           const std::vector<std::pair<std::string, size_t>>& chromosomeLengths,
           const std::string& bam_file)
{
  std::deque<std::future<std::vector<CountH5Record>>> future_counts;

  for (const auto& chromosomeLength : chromosomeLengths)
  {
    future_counts.push_back(std::async(count,
                                       chromosomeLength.first,
                                       cell_type,
                                       bin_size,
                                       chromosomeLength.second,
                                       bam_file));
  }

  const size_t record_size = sizeof(CountH5Record);

  size_t record_offset[] = { HOFFSET(CountH5Record, cell_type),
                             HOFFSET(CountH5Record, file_name),
                             HOFFSET(CountH5Record, chromosome),
                             HOFFSET(CountH5Record, bin_number),
                             HOFFSET(CountH5Record, count),
                             HOFFSET(CountH5Record, normalized_count) };

  size_t field_sizes[] = { sizeof(CountH5Record::cell_type),
                           sizeof(CountH5Record::file_name),
                           sizeof(CountH5Record::chromosome),
                           sizeof(CountH5Record::bin_number),
                           sizeof(CountH5Record::count),
                           sizeof(CountH5Record::normalized_count) };

  for (auto& future_count : future_counts)
  {
    std::vector<CountH5Record> records = future_count.get();
   
    herr_t status = H5TBappend_records(file, "bin_counts", records.size(), record_size,
                                       record_offset, field_sizes, records.data());
    if (status != 0)
    {
      std::cerr << "Error appending record, status = " << status << std::endl;
    }
  }
}

int main(int argc, char* argv[])
{
  try
  {
    if (argc <= 5 || argc % 2 != 1)
    {
      std::cerr << "usage: " << argv[0] << " cell_type bin_size bam_file hdf5_file chr1 lenght1 ... \n"
        << "\ne.g. " << argv[0] << " mm1s 100000 /ifs/hg18/mm1s/04032013_D1L57ACXX_4.TTAGGC.hg18.bwt.sorted.bam "
        << "chr1 247249719 chr2 242951149 chr3 199501827"
        << "\nnote that this application is intended to be run from bamliquidator_batch.py -- see"
        << "\nhttps://github.com/BradnerLab/pipeline/wiki for more information"
        << std::endl;
      return 1;
    }
    std::vector<std::pair<std::string, size_t>> chromosomeLengths;
    for (int arg = 5; arg < argc && arg + 1 < argc; arg += 2)
    {
      chromosomeLengths.push_back(
        std::make_pair(argv[arg], boost::lexical_cast<size_t>(argv[arg+1])));
    }

    const std::string cell_type = argv[1];
    const unsigned int bin_size = boost::lexical_cast<unsigned int>(argv[2]);
    const std::string bam_file_path = argv[3];
    const std::string hdf5_file_path = argv[4];

    if (bin_size == 0)
    {
      std::cerr << "bin size cannot be zero" << std::endl;
      return 2;
    }

    hid_t h5file = H5Fopen(hdf5_file_path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
    if (h5file < 0)
    {
      std::cerr << "Failed to open H5 file " << hdf5_file_path << std::endl;
      return 3;
    }

    batch(h5file, cell_type, bin_size, chromosomeLengths, bam_file_path);
   
    H5Fclose(h5file);

    return 0;
  }
  catch(const std::exception& e)
  {
    std::cerr << "Unhandled exception: " << e.what() << std::endl;

    return 4; 
  }
}

/* The MIT License (MIT) 

   Copyright (c) 2013 John DiMatteo (jdimatteo@gmail.com)

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE. 
 */
