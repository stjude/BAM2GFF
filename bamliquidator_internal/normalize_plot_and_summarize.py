#!/usr/bin/env python

##################################################################################
# The MIT License (MIT)
#
# Copyright (c) 2013 John DiMatteo (jdimatteo@gmail.com)
#
# Permission is hereby granted, free of charge, to any person obtaining a copy of
# this software and associated documentation files (the "Software"), to deal in
# the Software without restriction, including without limitation the rights to
# use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
# the Software, and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
# FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
# COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
# IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#
##################################################################################

from __future__ import division

# todo: rename summarizer.py, and move normalize(region... logic into multi-threaded bamliquidator_regions.m.cpp

import sys
import os
import argparse
import bokeh.plotting as bp
import tables
import scipy.stats as stats
import collections

# note that my initial version didn't do any flush calls, which lead to bogus rows being added
# to the normalized_counts table (which was evident when the normalized counts <= 95 + > 95 didn't add up right)
# -- I should probably look into why flush was necessary and/or file a bug with pytables

def delete_all_but_bin_counts_table(h5file):
    for table in h5file.root:
        if table.name != "bin_counts":
            for index in table.colindexes.values():
                index.column.remove_index()
            table.remove()

# todo: rename percentile table
def create_normalized_counts_table(h5file):
    class BinCount(tables.IsDescription):
        bin_number = tables.UInt32Col(    pos=0)
        cell_type  = tables.StringCol(16, pos=1)
        chromosome = tables.StringCol(16, pos=2)
        percentile = tables.Float64Col(   pos=4)
        file_name  = tables.StringCol(64, pos=5)

    table = h5file.create_table("/", "normalized_counts", BinCount, "normalized bin counts")

    table.flush()

    return table

def all_cell_types(counts):
    types = set()

    for row in counts:
        types.add(row["cell_type"])

    return types 

def all_chromosomes(counts):
    chromosomes = collections.OrderedDict() 

    for row in counts:
        chromosomes[row["chromosome"]] = None

    return chromosomes.keys() 

file_names_memo = {}
def file_names(counts, cell_type):
    if not cell_type in file_names_memo:
        file_names = set() 
       
        #print "Getting file names for cell type " + cell_type
        for row in counts.where("cell_type == '%s'" % cell_type):
            file_names.add(row["file_name"])

        file_names_memo[cell_type] = file_names

        #print "memoizing files for " + cell_type + ": " + str(file_names_memo[cell_type]) 
        
    return file_names_memo[cell_type] 

# todo: delete one of these functions (above and below)/
def file_names_in_cell_type(normalized_counts, cell_type):
    file_names = set()
    for row in normalized_counts.where("(file_name != '*') & (cell_type == '%s')" % cell_type):
        file_names.add(row["file_name"])
    return list(file_names)


def plot_summaries(output_directory, counts, chromosomes):
    bp.output_file(output_directory + "/summary.html")
    
    for chromosome in chromosomes:
        plot_summary(counts, chromosome)

    bp.save()

def plot_summary(counts, chromosome):
    #print " - plotting " + chromosome + " summary"

    condition = "(file_name == '*') & (chromosome == '%s')" % chromosome

    chromosome_count_by_bin = collections.defaultdict(int) 
    for row in counts.where(condition):
        chromosome_count_by_bin[row["bin_number"]] += row["normalized_count"]
    
    overall = bp.scatter(chromosome_count_by_bin.keys(), chromosome_count_by_bin.values())
    overall.title = chromosome + " counts per bin across all bam files"

def plot(output_directory, counts, chromosome, cell_types):
    bp.output_file(output_directory + "/" + chromosome + ".html")

    plot_summary(counts, chromosome)

    for cell_type in cell_types:
        #print " - plotting " + cell_type

        bin_number = [] 
        count = [] 
        
        condition = "(file_name == '*') & (chromosome == '%s') & (cell_type == '%s')" % (chromosome, cell_type)

        for row in counts.where(condition):
            bin_number.append(row["bin_number"])
            count.append(row["normalized_count"])

        cell_type_plot = bp.scatter(bin_number, count)
        cell_type_plot.title = "%s counts per bin" % cell_type 

    bp.save()

def normalize(region_counts, file_to_count):
    print "Normalizing"
     
    file_name = None

    for row in region_counts:
        if row["file_name"] != file_name:
            file_name = row["file_name"]
            total_count = file_to_count[file_name]
        
        region_size = row["stop"] - row["start"]
        factor = (1 / region_size) * (1 / (total_count / 10**6))

        row["normalized_count"] = row["count"] * factor 
        row.update()

    region_counts.flush()

# leave off file_name argument to calculate percentiles for the cell_type averaged normalized counts
def populate_percentiles(normalized_counts, counts, cell_type, file_name = "*"):
    bin_numbers = []
    normalized_count_list = []

    condition = "(cell_type == '%s') & (file_name == '%s')" % (cell_type, file_name)

    for row in counts.where(condition):
        bin_numbers.append(row["bin_number"])
        normalized_count_list.append(row["normalized_count"])

    percentiles = (stats.rankdata(normalized_count_list) - 1) / (len(normalized_count_list)-1) * 100
    # percentiles calculated in bulk as suggested at 
    # http://grokbase.com/t/python/python-list/092235vj27/faster-scipy-percentileofscore

    for i, row in enumerate(normalized_counts.where(condition)):
        assert bin_numbers[i] == row["bin_number"]
        row["percentile"] = percentiles[i]
        row.update()
    normalized_counts.flush()

# the cell type normalized counts are the averages of the genomes in the cell type
def populate_normalized_counts_for_cell_type(normalized_counts, cell_type, file_names):
    processed_a_single_file = False
    chromosome_to_summed_counts = collections.OrderedDict() 

    for file_name in file_names:
        condition = "(file_name == '%s') & (cell_type == '%s')" % (file_name, cell_type)
        for row in normalized_counts.where(condition):
            if processed_a_single_file:
                chromosome_to_summed_counts[row["chromosome"]][row["bin_number"]] += row["count"]
            else:
                if not chromosome_to_summed_counts.has_key(row["chromosome"]):
                    chromosome_to_summed_counts[row["chromosome"]] = []
                chromosome_to_summed_counts[row["chromosome"]].append(row["count"])
        processed_a_single_file = True
            
    cell_type_condition = "(file_name == '*') & (cell_type == '%s')" % cell_type
    prior_normalized_cell_type_rows_found = False
    for row in normalized_counts.where(cell_type_condition):
        prior_normalized_cell_type_rows_found = True
        break
    if not prior_normalized_cell_type_rows_found:
        # just add all the rows with -1 counts, so that the same code path for populating the counts
        # is used regardless of whether there were any prior normalized counts for this cell type
        for chromosome, summed_counts in chromosome_to_summed_counts.iteritems():
            for i, summed_count in enumerate(summed_counts):
                normalized_counts.row["bin_number"] = i
                normalized_counts.row["cell_type"] = cell_type 
                normalized_counts.row["chromosome"] = chromosome 
                normalized_counts.row["file_name"] = "*" 
                # using "*" instead of "" due to pytables empty string query bug
                # -- https://github.com/PyTables/PyTables/issues/184
                normalized_counts.row["count"] = -1 
                normalized_counts.row["percentile"] = -1
                normalized_counts.row.append()
        normalized_counts.flush()

    for row in normalized_counts.where(cell_type_condition):
        row["count"] = chromosome_to_summed_counts[row["chromosome"]][row["bin_number"]] / len(file_names)
        row.update()
    
    normalized_counts.flush()

def create_summary_table(h5file):
    class Summary(tables.IsDescription):
        bin_number = tables.UInt32Col(                    pos=0)
        chromosome = tables.StringCol(16,                 pos=2)
        avg_cell_type_percentile = tables.Float64Col(     pos=1)
        cell_types_gte_95th_percentile = tables.UInt32Col(pos=2)
        cell_types_lt_95th_percentile = tables.UInt32Col( pos=3)
        lines_gte_95th_percentile = tables.UInt32Col(     pos=4)
        lines_lt_95th_percentile = tables.UInt32Col(      pos=5)
        cell_types_gte_5th_percentile = tables.UInt32Col( pos=6)
        cell_types_lt_5th_percentile = tables.UInt32Col(  pos=7)
        lines_gte_5th_percentile = tables.UInt32Col(      pos=8)
        lines_lt_5th_percentile = tables.UInt32Col(       pos=9)

    table = h5file.create_table("/", "summary", Summary, "bin count summary")

    table.flush()

    return table
   

def populate_summary(summary, normalized_counts, chromosome):
    high = 95 # 95th percentile
    low  = 5  # 5th percentile

    summed_cell_type_percentiles_by_bin = collections.defaultdict(float) 
    cell_types_gte_high_percentile_by_bin = collections.defaultdict(int)
    cell_types_lt_high_percentile_by_bin = collections.defaultdict(int)
    lines_gte_high_percentile_by_bin = collections.defaultdict(int)
    lines_lt_high_percentile_by_bin = collections.defaultdict(int)
    cell_types_gte_low_percentile_by_bin = collections.defaultdict(int)
    cell_types_lt_low_percentile_by_bin = collections.defaultdict(int)
    lines_gte_low_percentile_by_bin = collections.defaultdict(int)
    lines_lt_low_percentile_by_bin = collections.defaultdict(int)
    lines = set()
    cell_types = set()
    max_bin = 0

    # note populating the dictionaries this way is much faster than looping through
    # each bin and finding the matching fraction rows
    for row in normalized_counts.where("chromosome == '%s'" % chromosome):
        bin_number = row["bin_number"]
        max_bin = max(max_bin, bin_number)
        percentile = row["percentile"]

        if row["file_name"] == "*":
            cell_types.add(row["cell_type"])
            summed_cell_type_percentiles_by_bin[bin_number] += percentile
            if percentile >= high:
                cell_types_gte_high_percentile_by_bin[bin_number] += 1
            else:
                cell_types_lt_high_percentile_by_bin[bin_number] += 1

            if percentile >= low:
                cell_types_gte_low_percentile_by_bin[bin_number] += 1
            else:
                cell_types_lt_low_percentile_by_bin[bin_number] += 1
        else:
            lines.add(row["file_name"])
            if percentile >= high:
                lines_gte_high_percentile_by_bin[bin_number] += 1
            else:
                lines_lt_high_percentile_by_bin[bin_number] += 1

            if percentile >= low:
                lines_gte_low_percentile_by_bin[bin_number] += 1
            else:
                lines_lt_low_percentile_by_bin[bin_number] += 1

    #print " - populating summary table with calculated summaries"

    for bin_number in xrange(max_bin+1):
        summary.row["bin_number"] = bin_number
        summary.row["chromosome"] = chromosome
        summary.row["avg_cell_type_percentile"] = summed_cell_type_percentiles_by_bin[bin_number] / len(cell_types)
        summary.row["cell_types_gte_95th_percentile"] = cell_types_gte_high_percentile_by_bin[bin_number]
        summary.row["cell_types_lt_95th_percentile"] = cell_types_lt_high_percentile_by_bin[bin_number]
        summary.row["lines_gte_95th_percentile"] = lines_gte_high_percentile_by_bin[bin_number]
        summary.row["lines_lt_95th_percentile"] = lines_lt_high_percentile_by_bin[bin_number]
        summary.row["cell_types_gte_5th_percentile"] = cell_types_gte_low_percentile_by_bin[bin_number]
        summary.row["cell_types_lt_5th_percentile"] = cell_types_lt_low_percentile_by_bin[bin_number]
        summary.row["lines_gte_5th_percentile"] = lines_gte_low_percentile_by_bin[bin_number]
        summary.row["lines_lt_5th_percentile"] = lines_lt_low_percentile_by_bin[bin_number]
        summary.row.append()
    summary.flush()

def normalize_plot_and_summarize(counts_file, output_directory, bin_size, file_to_count):
    counts = counts_file.root.bin_counts
    delete_all_but_bin_counts_table(counts_file)
    # regenerating these tables is quick and easier than updating prior records correctly

    normalized_counts = create_normalized_counts_table(counts_file)
    summary = create_summary_table(counts_file)

    cell_types = all_cell_types(counts)
    chromosomes = all_chromosomes(counts)

    skip_plots = False # useful if you are just experimenting with normalization and/or summary tables

    print "cell_types: %s" % ", ".join(cell_types)

    for cell_type in cell_types:
        print "Normalizing and calculating percentiles for cell type " + cell_type 
        current_file_names = file_names(counts, cell_type)
        for file_name in current_file_names:
           populate_percentiles(normalized_counts, counts, cell_type, file_name)
        populate_normalized_counts_for_cell_type(normalized_counts, cell_type, current_file_names) 
        populate_percentiles(normalized_counts, counts, cell_type)

    print "Indexing normalized counts"
    normalized_counts.cols.bin_number.create_index()
    normalized_counts.cols.percentile.create_index()
    normalized_counts.cols.file_name.create_index()
    normalized_counts.cols.chromosome.create_index()

    if not skip_plots:
        print "Plotting"
        for chromosome in chromosomes:
            plot(output_directory, counts, chromosome, cell_types)
        plot_summaries(output_directory, counts, chromosomes)

    print "Summarizing"
    for chromosome in chromosomes:
        populate_summary(summary, normalized_counts, chromosome)
    summary.cols.avg_cell_type_percentile.create_csindex()

    # Iterating over this index in reverse order is hundreds of times slower than iterating
    # in ascending order in my tests, but copying into a reverse sorted table is very fast.
    # So we create a sorted summary table sorted in decreasing percentile order.  If we need to
    # iterate in the reverse sorted order, than this sorted_summary table should be used.
    # Otherwise, we should use the summary table (including the case of ascending percentile
    # order, which is fast since the table is indexed by that column). See
    # https://groups.google.com/d/topic/pytables-users/EKMUxghQiPQ/discussion
    sorted_summary = summary.copy(newname="sorted_summary", sortby=summary.cols.avg_cell_type_percentile,
                                  step=-1, checkCSI=True,
                                  title="Summary table sorted in decreasing percentile order")
    sorted_summary.cols.bin_number.create_csindex()
