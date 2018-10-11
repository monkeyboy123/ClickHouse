#include <DataStreams/MergeSortingBlockInputStream.h>
#include <DataStreams/MergingSortedBlockInputStream.h>
#include <DataStreams/NativeBlockOutputStream.h>
#include <DataStreams/copyData.h>
#include <Common/formatReadable.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/CompressedWriteBuffer.h>
#include <Interpreters/sortBlock.h>


namespace ProfileEvents
{
    extern const Event ExternalSortWritePart;
    extern const Event ExternalSortMerge;
}

namespace DB
{


/** Remove constant columns from block.
  */
static void removeConstantsFromBlock(Block & block)
{
    size_t columns = block.columns();
    size_t i = 0;
    while (i < columns)
    {
        if (block.getByPosition(i).column->isColumnConst())
        {
            block.erase(i);
            --columns;
        }
        else
            ++i;
    }
}

static void removeConstantsFromSortDescription(const Block & header, SortDescription & description)
{
    description.erase(std::remove_if(description.begin(), description.end(),
        [&](const SortColumnDescription & elem)
        {
            if (!elem.column_name.empty())
                return header.getByName(elem.column_name).column->isColumnConst();
            else
                return header.safeGetByPosition(elem.column_number).column->isColumnConst();
        }), description.end());
}

/** Add into block, whose constant columns was removed by previous function,
  *  constant columns from header (which must have structure as before removal of constants from block).
  */
static void enrichBlockWithConstants(Block & block, const Block & header)
{
    size_t rows = block.rows();
    size_t columns = header.columns();

    for (size_t i = 0; i < columns; ++i)
    {
        const auto & col_type_name = header.getByPosition(i);
        if (col_type_name.column->isColumnConst())
            block.insert(i, {col_type_name.column->cloneResized(rows), col_type_name.type, col_type_name.name});
    }
}


MergeSortingBlockInputStream::MergeSortingBlockInputStream(
    const BlockInputStreamPtr & input, SortDescription & description_,
    size_t max_merged_block_size_, size_t limit_, size_t max_bytes_before_remerge_,
    size_t max_bytes_before_external_sort_, const std::string & tmp_path_)
    : description(description_), max_merged_block_size(max_merged_block_size_), limit(limit_),
    max_bytes_before_remerge(max_bytes_before_remerge_),
    max_bytes_before_external_sort(max_bytes_before_external_sort_), tmp_path(tmp_path_)
{
    children.push_back(input);
    header = children.at(0)->getHeader();
    header_without_constants = header;
    removeConstantsFromBlock(header_without_constants);
    removeConstantsFromSortDescription(header, description);
}


Block MergeSortingBlockInputStream::readImpl()
{
    /** Algorithm:
      * - read to memory blocks from source stream;
      * - if too many of them and if external sorting is enabled,
      *   - merge all blocks to sorted stream and write it to temporary file;
      * - at the end, merge all sorted streams from temporary files and also from rest of blocks in memory.
      */

    /// If has not read source blocks.
    if (!impl)
    {
        while (Block block = children.back()->read())
        {
            /// If there were only const columns in sort description, then there is no need to sort.
            /// Return the blocks as is.
            if (description.empty())
                return block;

            removeConstantsFromBlock(block);

            blocks.push_back(block);
            sum_rows_in_blocks += block.rows();
            sum_bytes_in_blocks += block.allocatedBytes();

            /** If significant amount of data was accumulated, perform preliminary merging step.
              */
            if (blocks.size() > 1
                && limit
                && limit * 2 < sum_rows_in_blocks   /// 2 is just a guess.
                && remerge_is_useful
                && max_bytes_before_remerge
                && sum_bytes_in_blocks > max_bytes_before_remerge)
            {
                remerge();
            }

            /** If too many of them and if external sorting is enabled,
              *  will merge blocks that we have in memory at this moment and write merged stream to temporary (compressed) file.
              * NOTE. It's possible to check free space in filesystem.
              */
            if (max_bytes_before_external_sort && sum_bytes_in_blocks > max_bytes_before_external_sort)
            {
                Poco::File(tmp_path).createDirectories();
                temporary_files.emplace_back(new Poco::TemporaryFile(tmp_path));
                const std::string & path = temporary_files.back()->path();
                WriteBufferFromFile file_buf(path);
                CompressedWriteBuffer compressed_buf(file_buf);
                NativeBlockOutputStream block_out(compressed_buf, 0, header_without_constants);
                MergeSortingBlocksBlockInputStream block_in(blocks, description, max_merged_block_size, limit);

                LOG_INFO(log, "Sorting and writing part of data into temporary file " + path);
                ProfileEvents::increment(ProfileEvents::ExternalSortWritePart);
                copyData(block_in, block_out, &is_cancelled);    /// NOTE. Possibly limit disk usage.
                LOG_INFO(log, "Done writing part of data into temporary file " + path);

                blocks.clear();
                sum_bytes_in_blocks = 0;
                sum_rows_in_blocks = 0;
            }
        }

        if ((blocks.empty() && temporary_files.empty()) || isCancelledOrThrowIfKilled())
            return Block();

        if (temporary_files.empty())
        {
            impl = std::make_unique<MergeSortingBlocksBlockInputStream>(blocks, description, max_merged_block_size, limit);
        }
        else
        {
            /// If there was temporary files.
            ProfileEvents::increment(ProfileEvents::ExternalSortMerge);

            LOG_INFO(log, "There are " << temporary_files.size() << " temporary sorted parts to merge.");

            /// Create sorted streams to merge.
            for (const auto & file : temporary_files)
            {
                temporary_inputs.emplace_back(std::make_unique<TemporaryFileStream>(file->path(), header_without_constants));
                inputs_to_merge.emplace_back(temporary_inputs.back()->block_in);
            }

            /// Rest of blocks in memory.
            if (!blocks.empty())
                inputs_to_merge.emplace_back(std::make_shared<MergeSortingBlocksBlockInputStream>(blocks, description, max_merged_block_size, limit));

            /// Will merge that sorted streams.
            impl = std::make_unique<MergingSortedBlockInputStream>(inputs_to_merge, description, max_merged_block_size, limit);
        }
    }

    Block res = impl->read();
    if (res)
        enrichBlockWithConstants(res, header);
    return res;
}


MergeSortingBlocksBlockInputStream::MergeSortingBlocksBlockInputStream(
    Blocks & blocks_, SortDescription & description_, size_t max_merged_block_size_, size_t limit_)
    : blocks(blocks_), header(blocks.at(0).cloneEmpty()), description(description_), max_merged_block_size(max_merged_block_size_), limit(limit_)
{
    Blocks nonempty_blocks;
    for (const auto & block : blocks)
    {
        if (block.rows() == 0)
            continue;

        nonempty_blocks.push_back(block);
        cursors.emplace_back(block, description);
        has_collation |= cursors.back().has_collation;
    }

    blocks.swap(nonempty_blocks);

    if (!has_collation)
    {
        for (size_t i = 0; i < cursors.size(); ++i)
            queue_without_collation.push(SortCursor(&cursors[i]));
    }
    else
    {
        for (size_t i = 0; i < cursors.size(); ++i)
            queue_with_collation.push(SortCursorWithCollation(&cursors[i]));
    }
}


Block MergeSortingBlocksBlockInputStream::readImpl()
{
    if (blocks.empty())
        return Block();

    if (blocks.size() == 1)
    {
        Block res = blocks[0];
        blocks.clear();
        return res;
    }

    return !has_collation
        ? mergeImpl<SortCursor>(queue_without_collation)
        : mergeImpl<SortCursorWithCollation>(queue_with_collation);
}


template <typename TSortCursor>
Block MergeSortingBlocksBlockInputStream::mergeImpl(std::priority_queue<TSortCursor> & queue)
{
    size_t num_columns = blocks[0].columns();

    MutableColumns merged_columns = blocks[0].cloneEmptyColumns();
    /// TODO: reserve (in each column)

    /// Take rows from queue in right order and push to 'merged'.
    size_t merged_rows = 0;
    while (!queue.empty())
    {
        TSortCursor current = queue.top();
        queue.pop();

        for (size_t i = 0; i < num_columns; ++i)
            merged_columns[i]->insertFrom(*current->all_columns[i], current->pos);

        if (!current->isLast())
        {
            current->next();
            queue.push(current);
        }

        ++total_merged_rows;
        if (limit && total_merged_rows == limit)
        {
            auto res = blocks[0].cloneWithColumns(std::move(merged_columns));
            blocks.clear();
            return res;
        }

        ++merged_rows;
        if (merged_rows == max_merged_block_size)
            return blocks[0].cloneWithColumns(std::move(merged_columns));
    }

    if (merged_rows == 0)
        return {};

    return blocks[0].cloneWithColumns(std::move(merged_columns));
}


void MergeSortingBlockInputStream::remerge()
{
    LOG_DEBUG(log, "Re-merging intermediate ORDER BY data (" << blocks.size() << " blocks with " << sum_rows_in_blocks << " rows) to save memory consumption");

    /// NOTE Maybe concat all blocks and partial sort will be faster than merge?
    MergeSortingBlocksBlockInputStream merger(blocks, description, max_merged_block_size, limit);

    Blocks new_blocks;
    size_t new_sum_rows_in_blocks = 0;
    size_t new_sum_bytes_in_blocks = 0;

    merger.readPrefix();
    while (Block block = merger.read())
    {
        new_sum_rows_in_blocks += block.rows();
        new_sum_bytes_in_blocks += block.allocatedBytes();
        new_blocks.emplace_back(std::move(block));
    }
    merger.readSuffix();

    LOG_DEBUG(log, "Memory usage is lowered from "
        << formatReadableSizeWithBinarySuffix(sum_bytes_in_blocks) << " to "
        << formatReadableSizeWithBinarySuffix(new_sum_bytes_in_blocks));

    /// If the memory consumption was not lowered enough - we will not perform remerge anymore. 2 is a guess.
    if (new_sum_bytes_in_blocks * 2 > sum_bytes_in_blocks)
        remerge_is_useful = false;

    blocks = std::move(new_blocks);
    sum_rows_in_blocks = new_sum_rows_in_blocks;
    sum_bytes_in_blocks = new_sum_bytes_in_blocks;
}


FinishSortingBlockInputStream::FinishSortingBlockInputStream(
    const BlockInputStreamPtr & input, SortDescription & description_sorted_,
    SortDescription & description_to_sort_,
    size_t max_merged_block_size_, size_t limit_)
    : description_sorted(description_sorted_), description_to_sort(description_to_sort_),
    max_merged_block_size(max_merged_block_size_), limit(limit_)
{
    children.push_back(input);
    header = children.at(0)->getHeader();
    removeConstantsFromSortDescription(header, description_sorted);
    removeConstantsFromSortDescription(header, description_to_sort);
}


struct Less
{
    const ColumnsWithSortDescriptions & left_columns;
    const ColumnsWithSortDescriptions & right_columns;

    Less(const ColumnsWithSortDescriptions & left_columns_, const ColumnsWithSortDescriptions & right_columns_) :
        left_columns(left_columns_), right_columns(right_columns_) {}

    bool operator() (size_t a, size_t b) const
    {
        for (auto it = left_columns.begin(), jt = right_columns.begin(); it != left_columns.end(); ++it, ++jt)
        {
            int res = it->second.direction * it->first->compareAt(a, b, *jt->first, it->second.nulls_direction);
            if (res < 0)
                return true;
            else if (res > 0)
                return false;
        }
        return false;
    }
};

Block FinishSortingBlockInputStream::readImpl()
{
    if (limit && total_rows_processed == limit)
        return {};

    Block res;
    if (impl)
        res = impl->read();

    /// If res block is empty, we finish sorting previous chunk of blocks.
    if (!res)
    {
        if (end_of_stream)
            return {};

        blocks.clear();
        if (tail_block)
            blocks.push_back(std::move(tail_block));

        Block block;
        size_t tail_pos = 0;
        while (true)
        {
            block = children.back()->read();

            /// End of input stream, but we can`t return immediatly, we need to merge already read blocks.
            /// Check it later, when get end of stream from impl.
            if (!block)
            {
                end_of_stream = true;
                break;
            }

            // If there were only const columns in sort description, then there is no need to sort.
            // Return the blocks as is.
            if (description_to_sort.empty())
                return block;

            size_t size = block.rows();
            if (size == 0)
                continue;

            removeConstantsFromBlock(block);

            /// Find the position of last already read key in current block.
            if (!blocks.empty())
            {
                const Block & last_block = blocks.back();
                auto last_columns = getColumnsWithSortDescription(last_block, description_sorted);
                auto current_columns = getColumnsWithSortDescription(block, description_sorted);

                Less less(last_columns, current_columns);

                IColumn::Permutation perm(size);
                for (size_t i = 0; i < size; ++i)
                    perm[i] = i;

                auto it = std::upper_bound(perm.begin(), perm.end(), last_block.rows() - 1, less);
                if (it != perm.end())
                {
                    tail_pos = it - perm.begin();
                    break;
                }
            }

            /// If we reach here, that means that current block is first in chunk
            /// or it all consists of rows with the same key as tail of a previous block.
            blocks.push_back(block);
        }

        /// We need to save tail of block, because next block may starts with the same key as in tail
        /// and we should sort these rows in one chunk.
        if (block)
        {
            Block head_block = block.cloneEmpty();
            tail_block = block.cloneEmpty();
            for (size_t i = 0; i < block.columns(); ++i)
            {
                head_block.getByPosition(i).column = block.getByPosition(i).column->cut(0, tail_pos);
                tail_block.getByPosition(i).column = block.getByPosition(i).column->cut(tail_pos, block.rows() - tail_pos);
            }
            if (head_block.rows())
                blocks.push_back(head_block);
        }

        impl = std::make_unique<MergeSortingBlocksBlockInputStream>(blocks, description_to_sort, max_merged_block_size, limit);
        res = impl->read();
    }

    if (res)
        enrichBlockWithConstants(res, header);

    total_rows_processed += res.rows();

    return res;
}

}
