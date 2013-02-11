#include "stdinc.h"
#include "F4MatrixBuilder2.hpp"

#include "LogDomain.hpp"
#include <tbb/tbb.h>

MATHICGB_DEFINE_LOG_DOMAIN(
  F4MatrixBuild2,
  "Displays statistics about F4 matrix construction."
);

class F4MatrixBuilder2::F4PreBlock {
public:
  typedef uint32 RowIndex;
  typedef uint32 ColIndex;
  typedef coefficient ExternalScalar;
  typedef SparseMatrix::Scalar Scalar;

  struct Row {
    const ColIndex* indices;
    const Scalar* scalars;
    const ExternalScalar* externalScalars;
    ColIndex entryCount;
  };

  RowIndex rowCount() const {return static_cast<RowIndex>(mRows.size());}

  Row row(const RowIndex row) const {
    MATHICGB_ASSERT(row < mRows.size());
    const auto& r = mRows[row];
    Row rr;
    rr.indices = mIndices.data() + r.indicesBegin;
    rr.entryCount = r.entryCount;
    if (r.externalScalars == 0) {
      rr.scalars = mScalars.data() + r.scalarsBegin;
      rr.externalScalars = 0;
    } else {
      rr.scalars = 0;
      rr.externalScalars = r.externalScalars;
    }
    return rr;
  }

  ColIndex* makeRowWithTheseScalars(const Poly& scalars) {
    MATHICGB_ASSERT(rowCount() < std::numeric_limits<RowIndex>::max());
    MATHICGB_ASSERT
      (scalars.termCount() < std::numeric_limits<ColIndex>::max());

    InternalRow row;
    row.indicesBegin = mIndices.size();
    row.scalarsBegin = std::numeric_limits<decltype(row.scalarsBegin)>::max();
    row.entryCount = static_cast<ColIndex>(scalars.termCount());
    row.externalScalars = scalars.coefficientBegin();
    mRows.push_back(row);

    mIndices.resize(mIndices.size() + row.entryCount);
    return mIndices.data() + row.indicesBegin;
  }

  std::pair<ColIndex*, Scalar*> makeRow(ColIndex entryCount) {
    MATHICGB_ASSERT(rowCount() < std::numeric_limits<RowIndex>::max());

    InternalRow row;
    row.indicesBegin = mIndices.size();
    row.scalarsBegin = mScalars.size();
    row.entryCount = entryCount;
    row.externalScalars = 0;
    mRows.push_back(row);

    mIndices.resize(mIndices.size() + entryCount);
    mScalars.resize(mScalars.size() + entryCount);
    return std::make_pair(
      mIndices.data() + row.indicesBegin,
      mScalars.data() + row.scalarsBegin
    );
  }

  void removeLastEntries(const RowIndex row, const ColIndex count) {
    MATHICGB_ASSERT(row < rowCount());
    MATHICGB_ASSERT(mRows[row].entryCount >= count);
    mRows[row].entryCount -= count;
    if (row != rowCount() - 1)
      return;
    mIndices.resize(mIndices.size() - count);
    if (mRows[row].externalScalars == 0)
      mScalars.resize(mScalars.size() - count);
  }

private:
  struct InternalRow {
    size_t indicesBegin;
    size_t scalarsBegin;
    ColIndex entryCount;
    const ExternalScalar* externalScalars;
  };

  std::vector<ColIndex> mIndices;
  std::vector<Scalar> mScalars;
  std::vector<InternalRow> mRows;
};

MATHICGB_NO_INLINE
std::pair<F4MatrixBuilder2::ColIndex, ConstMonomial>
F4MatrixBuilder2::findOrCreateColumn(
  const const_monomial monoA,
  const const_monomial monoB,
  TaskFeeder& feeder
) {
  MATHICGB_ASSERT(!monoA.isNull());
  MATHICGB_ASSERT(!monoB.isNull());
  const auto col = ColReader(mMap).findProduct(monoA, monoB);
  if (col.first != 0)
    return std::make_pair(*col.first, col.second);
  return createColumn(monoA, monoB, feeder);
}

MATHICGB_INLINE
std::pair<F4MatrixBuilder2::ColIndex, ConstMonomial>
F4MatrixBuilder2::findOrCreateColumn(
  const const_monomial monoA,
  const const_monomial monoB,
  const ColReader& colMap,
  TaskFeeder& feeder
) {
  MATHICGB_ASSERT(!monoA.isNull());
  MATHICGB_ASSERT(!monoB.isNull());
  const auto col = colMap.findProduct(monoA, monoB);
  if (col.first == 0)
    return findOrCreateColumn(monoA, monoB, feeder);
  return std::make_pair(*col.first, col.second);
}

MATHICGB_NO_INLINE
void F4MatrixBuilder2::createTwoColumns(
  const const_monomial monoA1,
  const const_monomial monoA2,
  const const_monomial monoB,
  TaskFeeder& feeder
) {
  createColumn(monoA1, monoB, feeder);
  createColumn(monoA2, monoB, feeder);
}

F4MatrixBuilder2::F4MatrixBuilder2(
  const PolyBasis& basis,
  const size_t memoryQuantum
):
  mMemoryQuantum(memoryQuantum),
  mTmp(basis.ring().allocMonomial()),
  mBasis(basis),
  mMap(basis.ring())
{
  // This assert has to be _NO_ASSUME since otherwise the compiler will assume
  // that the error checking branch here cannot be taken and optimize it away.
  const Scalar maxScalar = std::numeric_limits<Scalar>::max();
  MATHICGB_ASSERT_NO_ASSUME(ring().charac() <= maxScalar);
  if (ring().charac() > maxScalar)
    mathic::reportInternalError("F4MatrixBuilder2: too large characteristic.");
}

void F4MatrixBuilder2::addSPolynomialToMatrix(
  const Poly& polyA,
  const Poly& polyB
) {
  MATHICGB_ASSERT(!polyA.isZero());
  MATHICGB_ASSERT(polyA.isMonic());
  MATHICGB_ASSERT(!polyB.isZero());
  MATHICGB_ASSERT(polyB.isMonic());

  RowTask task;
  task.poly = &polyA;
  task.sPairPoly = &polyB;
  mTodo.push_back(task);
}

void F4MatrixBuilder2::addPolynomialToMatrix(const Poly& poly) {
  if (poly.isZero())
    return;

  RowTask task = {};
  task.poly = &poly;
  mTodo.push_back(task);
}

void F4MatrixBuilder2::addPolynomialToMatrix
(const_monomial multiple, const Poly& poly) {
  MATHICGB_ASSERT(ring().hashValid(multiple));
  if (poly.isZero())
    return;

  RowTask task = {};
  task.poly = &poly;
  task.desiredLead = ring().allocMonomial();
  ring().monomialMult(poly.getLeadMonomial(), multiple, task.desiredLead);
  MATHICGB_ASSERT(ring().hashValid(task.desiredLead));

  MATHICGB_ASSERT(task.sPairPoly == 0);
  mTodo.push_back(task);
}

namespace {
  class ColumnComparer {
  public:
    ColumnComparer(const PolyRing& ring): mRing(ring) {}

    typedef SparseMatrix::ColIndex ColIndex;
    typedef std::pair<monomial, ColIndex> Pair;
    bool operator()(const Pair& a, const Pair b) const {
      return mRing.monomialLT(b.first, a.first);
    }

  private:
    const PolyRing& mRing;
  };

  std::vector<SparseMatrix::ColIndex> sortColumnMonomialsAndMakePermutation(
    std::vector<monomial>& monomials,
    const PolyRing& ring
  ) {
    typedef SparseMatrix::ColIndex ColIndex;
    MATHICGB_ASSERT(monomials.size() <= std::numeric_limits<ColIndex>::max());
    const ColIndex colCount = static_cast<ColIndex>(monomials.size());
    // Monomial needs to be non-const as we are going to put these
    // monomials back into the vector of monomials which is not const.
    std::vector<std::pair<monomial, ColIndex>> columns;
    columns.reserve(colCount);
    for (ColIndex col = 0; col < colCount; ++col)
      columns.push_back(std::make_pair(monomials[col], col));
    std::sort(columns.begin(), columns.end(), ColumnComparer(ring));

    // Apply sorting permutation to monomials. This is why it is necessary to
    // copy the values in monomial out of there: in-place application of a
    // permutation is messy.
    MATHICGB_ASSERT(columns.size() == colCount);
    MATHICGB_ASSERT(monomials.size() == colCount);
    for (size_t col = 0; col < colCount; ++col) {
      MATHICGB_ASSERT(col == 0 ||
        ring.monomialLT(columns[col].first, columns[col - 1].first));
      monomials[col] = columns[col].first;
    }

    // Construct permutation of indices to match permutation of monomials
    std::vector<ColIndex> permutation(colCount);
    for (ColIndex col = 0; col < colCount; ++col) {
      // The monomial for column columns[col].second is now the
      // monomial for col, so we need the inverse map for indices.
      permutation[columns[col].second] = col;
    }

    return std::move(permutation);
  }

  class LeftRightProjection {
  public:
    typedef SparseMatrix::ColIndex ColIndex;

    LeftRightProjection(
      const std::vector<char>& isColToLeft,
      const MonomialMap<ColIndex>& map
    ) {
      const auto& ring = map.ring();

      // Sort columns by monomial while keeping track of original index.
      MonomialMap<ColIndex>::Reader reader(map);
      typedef std::pair<ColIndex, ConstMonomial> IndexMono;
      std::vector<IndexMono> columns(reader.begin(), reader.end());
      const auto cmp = [&ring](const IndexMono& a, const IndexMono b) {
        return ring.monomialLT(b.second, a.second);
      };
      tbb::parallel_sort(columns.begin(), columns.end(), cmp);

      // Copy monomials and construct projection mapping.
      MATHICGB_ASSERT
        (isColToLeft.size() <= std::numeric_limits<ColIndex>::max());
      ColIndex colCount = static_cast<ColIndex>(isColToLeft.size());
      mProject.resize(isColToLeft.size());
      for (size_t i = 0; i < colCount; ++i) {
        const auto indexMono = columns[i];
        monomial mono = ring.allocMonomial();
        ring.monomialCopy(indexMono.second, mono);

        auto& projected = mProject[indexMono.first];
        projected.left = isColToLeft[indexMono.first];
        if (projected.left) {
          projected.index = static_cast<ColIndex>(mLeftMonomials.size());
          mLeftMonomials.push_back(mono);
        } else {
          projected.index = static_cast<ColIndex>(mRightMonomials.size());
          mRightMonomials.push_back(mono);
        }
      }
      MATHICGB_ASSERT
        (mLeftMonomials.size() + mRightMonomials.size() == isColToLeft.size());
    }

    struct Projected {
      ColIndex index;
      bool left;
    };

    Projected project(const ColIndex index) const {
      MATHICGB_ASSERT(index < mProject.size());
      return mProject[index];
    }

    void project(
      const std::vector<F4MatrixBuilder2::F4PreBlock*>& preBlocks,
      SparseMatrix& left,
      SparseMatrix& right,
      const PolyRing& ring
    ) const {
      left.clear();
      right.clear();
      const auto modulus = static_cast<SparseMatrix::Scalar>(ring.charac());

      const auto end = preBlocks.end();
      for (auto it = preBlocks.begin(); it != end; ++it) {
        auto& block = **it;
        const auto rowCount = block.rowCount();
        for (SparseMatrix::RowIndex r = 0; r < rowCount; ++r) {
          const auto row = block.row(r);
          if (row.entryCount == 0)
            continue;
          MATHICGB_ASSERT(row.entryCount != 0);
          MATHICGB_ASSERT(row.scalars == 0 || row.externalScalars == 0);

          if (row.externalScalars != 0) {
            auto indices = row.indices;
            auto indicesEnd = row.indices + row.entryCount;
            auto scalars = row.externalScalars;
            for (; indices != indicesEnd; ++indices, ++scalars) {
              const auto scalar = static_cast<SparseMatrix::Scalar>(*scalars);
              const auto index = *indices;
              const auto translated = project(index);
              if (translated.left)
                left.appendEntry(translated.index, scalar);
              else
                right.appendEntry(translated.index, scalar);
            }
          } else {
            auto indices = row.indices;
            auto indicesEnd = row.indices + row.entryCount;
            auto scalars = row.scalars;
            for (; indices != indicesEnd; ++indices, ++scalars) {
              const auto index = *indices;
              const auto translated = project(index);
              if (translated.left)
                left.appendEntry(translated.index, *scalars);
              else
                right.appendEntry(translated.index, *scalars);
            }
          }
          MATHICGB_ASSERT(left.rowCount() == right.rowCount());
          left.rowDone();
          right.rowDone();
        }
      }
    }

    void project(
      const std::vector<std::pair<SparseMatrix::Scalar,F4MatrixBuilder2::F4PreBlock::Row>>& from,
      SparseMatrix& left,
      SparseMatrix& right,
      const PolyRing& ring
    ) const {
      left.clear();
      right.clear();
      const auto fromEnd = from.end();
      for (auto fromIt = from.begin(); fromIt != fromEnd; ++fromIt) {
        const auto row = fromIt->second;
        MATHICGB_ASSERT(row.entryCount != 0);
        MATHICGB_ASSERT(row.scalars == 0 || row.externalScalars == 0);
        const auto modulus = static_cast<SparseMatrix::Scalar>(ring.charac());

        if (row.externalScalars != 0) {
          auto indices = row.indices;
          auto indicesEnd = row.indices + row.entryCount;
          auto scalars = row.externalScalars;
          for (; indices != indicesEnd; ++indices, ++scalars) {
            const auto scalar = static_cast<SparseMatrix::Scalar>(*scalars);
            const auto index = *indices;
            const auto translated = project(index);
            if (translated.left)
              left.appendEntry(translated.index, scalar);
            else
              right.appendEntry(translated.index, scalar);
          }
        } else {
          auto indices = row.indices;
          auto indicesEnd = row.indices + row.entryCount;
          auto scalars = row.scalars;
          for (; indices != indicesEnd; ++indices, ++scalars) {
            const auto index = *indices;
            const auto translated = project(index);
            if (translated.left)
              left.appendEntry(translated.index, *scalars);
            else
              right.appendEntry(translated.index, *scalars);
          }
        }
        const auto rowIndex = left.rowCount();
        MATHICGB_ASSERT(rowIndex == right.rowCount());
        left.rowDone();
        right.rowDone();

        if (fromIt->first != 1) {
          MATHICGB_ASSERT(fromIt->first != 0);
          left.multiplyRow(rowIndex, fromIt->first, modulus);
          right.multiplyRow(rowIndex, fromIt->first, modulus);
          MATHICGB_ASSERT(left.rowBegin(rowIndex).scalar() == 1);
        }

        MATHICGB_ASSERT(left.rowCount() == right.rowCount());
      }
    }

    const std::vector<monomial>& leftMonomials() const {
      return mLeftMonomials;
    }

    std::vector<monomial> moveLeftMonomials() {
      return std::move(mLeftMonomials);
    }

    std::vector<monomial> moveRightMonomials() {
      return std::move(mRightMonomials);
    }

  private:
    std::vector<Projected> mProject;
    std::vector<monomial> mLeftMonomials;
    std::vector<monomial> mRightMonomials;
  };

  class TopBottomProjectionLate {
  public:
    TopBottomProjectionLate(
      const SparseMatrix& left,
      const SparseMatrix& right,
      const SparseMatrix::ColIndex leftColCount,
      const PolyRing& ring
    ):
      mModulus(static_cast<SparseMatrix::Scalar>(ring.charac()))
    {
      const auto noRow = std::numeric_limits<SparseMatrix::ColIndex>::max();
      mTopRows.resize(leftColCount, std::make_pair(0, noRow));

      MATHICGB_ASSERT(left.computeColCount() == leftColCount);
      MATHICGB_ASSERT(left.rowCount() >= leftColCount);
      MATHICGB_ASSERT(left.rowCount() == right.rowCount());

      std::vector<SparseMatrix::ColIndex> topEntryCounts(leftColCount);

      const auto rowCount = left.rowCount();
      for (SparseMatrix::RowIndex row = 0; row < rowCount; ++row) {
        const auto leftEntryCount = left.entryCountInRow(row);
        const auto entryCount = leftEntryCount + right.entryCountInRow(row);
        MATHICGB_ASSERT(entryCount >= leftEntryCount); // no overflow
        if (entryCount == 0)
          continue; // ignore zero rows
        if (leftEntryCount == 0) {
          mBottomRows.push_back(std::make_pair(1, row)); //can't be top/reducer
          continue;
        }
        const auto lead = left.rowBegin(row).index();
        if (mTopRows[lead].second != noRow && topEntryCounts[lead]<entryCount)
          mBottomRows.push_back(std::make_pair(1, row)); //other reducer better
        else {
          if (mTopRows[lead].second != noRow)
            mBottomRows.push_back(std::make_pair(1, mTopRows[lead].second));
          topEntryCounts[lead] = entryCount;
          mTopRows[lead].second = row; // do scalar .first later
        }
      }

      const auto modulus = static_cast<SparseMatrix::Scalar>(ring.charac());
      for (SparseMatrix::RowIndex r = 0; r < leftColCount; ++r) {
        const auto row = mTopRows[r].second;
        MATHICGB_ASSERT(row != noRow);
        MATHICGB_ASSERT(left.entryCountInRow(row) > 0);
        MATHICGB_ASSERT(left.rowBegin(row).index() == r);
        MATHICGB_ASSERT(left.rowBegin(row).scalar() != 0);
        MATHICGB_ASSERT(topEntryCounts[r] ==
          left.entryCountInRow(row) + right.entryCountInRow(row));

        const auto leadScalar = left.rowBegin(row).scalar();
        mTopRows[r].first = leadScalar == 1 ? 1 : // 1 is the common case
          modularInverse(leadScalar, modulus);
      }

#ifdef MATHICGB_DEBUG
      for (SparseMatrix::RowIndex r = 0; r < mBottomRows.size(); ++r) {
        const auto row = mBottomRows[r].second;
        MATHICGB_ASSERT(
          left.entryCountInRow(row) + right.entryCountInRow(row) > 0);
        MATHICGB_ASSERT(mBottomRows[r].first == 1);
      }
#endif
    }

    void project(
      SparseMatrix&& in,
      SparseMatrix& top,
      SparseMatrix& bottom
    ) {
      top.clear();
      bottom.clear();

      const auto rowCountTop =
        static_cast<SparseMatrix::RowIndex>(mTopRows.size());
      for (SparseMatrix::RowIndex toRow = 0; toRow < rowCountTop; ++toRow) {
        top.appendRow(in, mTopRows[toRow].second);
        if (mTopRows[toRow].first != 1)
          top.multiplyRow(toRow, mTopRows[toRow].first, mModulus);
      }

      const auto rowCountBottom =
        static_cast<SparseMatrix::RowIndex>(mBottomRows.size());
      for (SparseMatrix::RowIndex toRow = 0; toRow < rowCountBottom; ++toRow) {
        bottom.appendRow(in, mBottomRows[toRow].second);
        if (mBottomRows[toRow].first != 1)
            bottom.multiplyRow(toRow, mBottomRows[toRow].first, mModulus);
      }

      in.clear();
    }

  private:
    const SparseMatrix::Scalar mModulus;
    std::vector<std::pair<SparseMatrix::Scalar, SparseMatrix::RowIndex>> mTopRows;
    std::vector<std::pair<SparseMatrix::Scalar, SparseMatrix::RowIndex>> mBottomRows;
  };

  class TopBottomProjection {
  public:
    TopBottomProjection(
      const std::vector<F4MatrixBuilder2::F4PreBlock*>& blocks,
      const LeftRightProjection& leftRight,
      const PolyRing& ring
    ):
      mReducerRows(leftRight.leftMonomials().size())
    {
      typedef SparseMatrix::RowIndex RowIndex;
      const auto noReducer = std::numeric_limits<RowIndex>::max();
      F4PreBlock::Row noRow = {};
      noRow.indices = 0;
      const auto noCol = std::numeric_limits<SparseMatrix::ColIndex>::max();

      const auto modulus = static_cast<SparseMatrix::Scalar>(ring.charac());

      const auto end = blocks.end();
      for (auto it = blocks.begin(); it != end; ++it) {
        auto& block = **it;
        const auto rowCount = block.rowCount();
        for (RowIndex r = 0; r < rowCount; ++r) {
          const auto row = block.row(r);
          if (row.entryCount == 0)
            continue;

          // Determine leading (minimum index) left entry.
          const auto lead = [&] {
            for (SparseMatrix::ColIndex col = 0; col < row.entryCount; ++col) {
              auto const translated = leftRight.project(row.indices[col]);
              if (translated.left)
                return std::make_pair(col, translated.index);
            }
            return std::make_pair(noCol, noCol);
          }();
          // Decide if this should be a reducer or reducee row.
          if (lead.second == noCol) {
            mReduceeRows.push_back(std::make_pair(1, row)); // no left entries
            continue;
          }
          MATHICGB_ASSERT(row.scalars != 0 || row.externalScalars != 0);

          const auto reducer = mReducerRows[lead.second].second;
          if (
            reducer.entryCount != 0 && // already have a reducer and...
            reducer.entryCount < row.entryCount // ...it is sparser/better
          )
            mReduceeRows.push_back(std::make_pair(1, row));
          else {
            if (reducer.entryCount != 0)
              mReduceeRows.push_back(std::make_pair(1, reducer));
            const auto leadScalar = row.scalars != 0 ? row.scalars[lead.first] :
              static_cast<SparseMatrix::Scalar>
                (row.externalScalars[lead.first]);
            MATHICGB_ASSERT(leadScalar != 0);
            const auto inverse = leadScalar == 1 ?
              1 : modularInverse(leadScalar, modulus);
            mReducerRows[lead.second] = std::make_pair(inverse, row);
          }
        }
      }

#ifdef MATHICGB_DEBUG
  for (size_t  i = 0; i < mReducerRows.size(); ++i) {
    const auto row = mReducerRows[i];
    MATHICGB_ASSERT(row.second.entryCount > 0);
    for (SparseMatrix::ColIndex col = 0; ; ++col) {
      MATHICGB_ASSERT(col < row.second.entryCount);
      const auto projected = leftRight.project(row.second.indices[col]);
      if (projected.left) {
        MATHICGB_ASSERT(projected.index == i);
        const auto leadScalar = row.second.scalars != 0 ?
          row.second.scalars[col] :
          static_cast<SparseMatrix::Scalar>(row.second.externalScalars[col]);
        MATHICGB_ASSERT(modularProduct(leadScalar, row.first, modulus) == 1);
        break;
      }
    }
  }
  for (size_t  i = 0; i < mReduceeRows.size(); ++i) {
    const auto row = mReduceeRows[i];
    MATHICGB_ASSERT(row.second.entryCount > 0);
    MATHICGB_ASSERT(row.first == 1);
  }
#endif
    }

    typedef F4MatrixBuilder2::F4PreBlock F4PreBlock;
    const std::vector<std::pair<SparseMatrix::Scalar,F4MatrixBuilder2::F4PreBlock::Row>>& reducerRows() const {
      return mReducerRows;
    }

    const std::vector<std::pair<SparseMatrix::Scalar,F4MatrixBuilder2::F4PreBlock::Row>>& reduceeRows() const {
      return mReduceeRows;
    }

  private:
    std::vector<std::pair<SparseMatrix::Scalar,F4MatrixBuilder2::F4PreBlock::Row>> mReducerRows;
    std::vector<std::pair<SparseMatrix::Scalar,F4MatrixBuilder2::F4PreBlock::Row>> mReduceeRows;
  };
}

void F4MatrixBuilder2::buildMatrixAndClear(QuadMatrix& quadMatrix) {
  MATHICGB_LOG_TIME(F4MatrixBuild2) <<
    "\n***** Constructing matrix *****\n";

  if (mTodo.empty()) {
    quadMatrix = QuadMatrix();
    quadMatrix.ring = &ring();
    return;
  }

  // Process pending rows until we are done. Note that the methods
  // we are calling here can add more pending items.

  struct ThreadData {
    F4PreBlock block;
    monomial tmp1;
    monomial tmp2;
  };

  tbb::enumerable_thread_specific<ThreadData> threadData([&](){  
    ThreadData data;
    {
      tbb::mutex::scoped_lock guard(mCreateColumnLock);
      data.tmp1 = ring().allocMonomial();
      data.tmp2 = ring().allocMonomial();
    }
    return std::move(data);
  });

  // Construct the matrix as pre-blocks
  tbb::parallel_do(mTodo.begin(), mTodo.end(),
    [&](const RowTask& task, TaskFeeder& feeder)
  {
    auto& data = threadData.local();
    const auto& poly = *task.poly;

    if (task.sPairPoly != 0) {
      ring().monomialColons(
        poly.getLeadMonomial(),
        task.sPairPoly->getLeadMonomial(),
        data.tmp2,
        data.tmp1
      );
      appendRowSPair
        (&poly, data.tmp1, task.sPairPoly, data.tmp2, data.block, feeder);
      return;
    }
    if (task.desiredLead.isNull())
      ring().monomialSetIdentity(data.tmp1);
    else
      ring().monomialDivide
        (task.desiredLead, poly.getLeadMonomial(), data.tmp1);
    MATHICGB_ASSERT(ring().hashValid(data.tmp1));
    appendRow(data.tmp1, *task.poly, data.block, feeder);
  });
  MATHICGB_ASSERT(!threadData.empty()); // as mTodo empty causes early return

  // Free the monomials from all the tasks
  const auto todoEnd = mTodo.end();
  for (auto it = mTodo.begin(); it != todoEnd; ++it)
    if (!it->desiredLead.isNull())
      ring().freeMonomial(it->desiredLead);
  mTodo.clear();

  // Collect pre-blocks from each thread
  std::vector<F4PreBlock*> blocks;
  blocks.reserve(threadData.size());
  const auto end = threadData.end();
  for (auto it = threadData.begin(); it != end; ++it) {
    blocks.emplace_back(&it->block);
    ring().freeMonomial(it->tmp1);
    ring().freeMonomial(it->tmp2);
    continue;
  }

  // Create projections
  LeftRightProjection projection(mIsColumnToLeft, mMap);
  mMap.clearNonConcurrent();

  if (true) {
    TopBottomProjection topBottom(blocks, projection, ring());

    // Project the pre-blocks into the matrix
    projection.project(topBottom.reducerRows(), quadMatrix.topLeft, quadMatrix.topRight, ring());
    projection.project(topBottom.reduceeRows(), quadMatrix.bottomLeft, quadMatrix.bottomRight, ring());
  } else {
    SparseMatrix left;
    SparseMatrix right;
    projection.project(blocks, left, right, ring());
    TopBottomProjectionLate topBottom(left, right, left.computeColCount(), ring());
    topBottom.project(std::move(left), quadMatrix.topLeft, quadMatrix.bottomLeft);
    topBottom.project(std::move(right), quadMatrix.topRight, quadMatrix.bottomRight);
  }

  quadMatrix.ring = &ring();
  quadMatrix.leftColumnMonomials = projection.moveLeftMonomials();
  quadMatrix.rightColumnMonomials = projection.moveRightMonomials();
  threadData.clear();

#ifdef MATHICGB_DEBUG
  for (size_t side = 0; side < 2; ++side) {
    auto& monos = side == 0 ?
      quadMatrix.leftColumnMonomials : quadMatrix.rightColumnMonomials;
    for (auto it = monos.begin(); it != monos.end(); ++it) {
      MATHICGB_ASSERT(!it->isNull());
    }
  }
  for (RowIndex row = 0; row < quadMatrix.topLeft.rowCount(); ++row) {
    MATHICGB_ASSERT(quadMatrix.topLeft.entryCountInRow(row) > 0);
    MATHICGB_ASSERT(quadMatrix.topLeft.leadCol(row) == row);
  }
  MATHICGB_ASSERT(quadMatrix.debugAssertValid());
#endif
}

std::pair<F4MatrixBuilder2::ColIndex, ConstMonomial>
F4MatrixBuilder2::createColumn(
  const const_monomial monoA,
  const const_monomial monoB,
  TaskFeeder& feeder
) {
  MATHICGB_ASSERT(!monoA.isNull());
  MATHICGB_ASSERT(!monoB.isNull());

  tbb::mutex::scoped_lock lock(mCreateColumnLock);
  // see if the column exists now after we have synchronized
  {
    const auto found(ColReader(mMap).findProduct(monoA, monoB));
    if (found.first != 0)
      return std::make_pair(*found.first, found.second);
  }

  // The column really does not exist, so we need to create it
  ring().monomialMult(monoA, monoB, mTmp);
  if (!ring().monomialHasAmpleCapacity(mTmp))
    mathic::reportError("Monomial exponent overflow in F4MatrixBuilder2.");
  MATHICGB_ASSERT(ring().hashValid(mTmp));

  // look for a reducer of mTmp
  const size_t reducerIndex = mBasis.classicReducer(mTmp);
  const bool insertLeft = (reducerIndex != static_cast<size_t>(-1));

  // Create the new left or right column
  if (mIsColumnToLeft.size() >= std::numeric_limits<ColIndex>::max())
    throw std::overflow_error("Too many columns in QuadMatrix");
  const auto newIndex = static_cast<ColIndex>(mIsColumnToLeft.size());
  const auto inserted = mMap.insert(std::make_pair(mTmp, newIndex));
  mIsColumnToLeft.push_back(insertLeft);

  // schedule new task if we found a reducer
  if (insertLeft) {
    RowTask task = {};
    task.poly = &mBasis.poly(reducerIndex);
    task.desiredLead = inserted.first.second.castAwayConst();
    feeder.add(task);
  }

  return std::make_pair(*inserted.first.first, inserted.first.second);
}

void F4MatrixBuilder2::appendRow(
  const const_monomial multiple,
  const Poly& poly,
  F4PreBlock& block,
  TaskFeeder& feeder
) {
  MATHICGB_ASSERT(!multiple.isNull());

  const auto begin = poly.begin();
  const auto end = poly.end();
  const auto count = std::distance(begin, end);
  MATHICGB_ASSERT(count < std::numeric_limits<ColIndex>::max());
  auto indices = block.makeRowWithTheseScalars(poly);

  auto it = begin;
  if ((count % 2) == 1) {
    ColReader reader(mMap);
    const auto col = findOrCreateColumn
      (it.getMonomial(), multiple, reader, feeder);
	MATHICGB_ASSERT(it.getCoefficient() < std::numeric_limits<Scalar>::max());
    MATHICGB_ASSERT(it.getCoefficient());
    //matrix.appendEntry(col.first, static_cast<Scalar>(it.getCoefficient()));
    *indices = col.first;
    ++indices;
    //*row.first++ = col.first;
    //*row.second++ = static_cast<Scalar>(it.getCoefficient());
    ++it;
  }
updateReader:
  ColReader colMap(mMap);
  MATHICGB_ASSERT((std::distance(it, end) % 2) == 0);
  while (it != end) {
	MATHICGB_ASSERT(it.getCoefficient() < std::numeric_limits<Scalar>::max());
    MATHICGB_ASSERT(it.getCoefficient() != 0);
    const auto scalar1 = static_cast<Scalar>(it.getCoefficient());
    const const_monomial mono1 = it.getMonomial();

    auto it2 = it;
    ++it2;
	MATHICGB_ASSERT(it2.getCoefficient() < std::numeric_limits<Scalar>::max());
    MATHICGB_ASSERT(it2.getCoefficient() != 0);
    const auto scalar2 = static_cast<Scalar>(it2.getCoefficient());
    const const_monomial mono2 = it2.getMonomial();

    const auto colPair = colMap.findTwoProducts(mono1, mono2, multiple);
    if (colPair.first == 0 || colPair.second == 0) {
      createTwoColumns(mono1, mono2, multiple, feeder);
      goto updateReader;
    }

    //matrix.appendEntry(*colPair.first, scalar1);
    //matrix.appendEntry(*colPair.second, scalar2);

    *indices = *colPair.first;
    ++indices;
    *indices = *colPair.second;
    ++indices;

    //*row.first++ = *colPair.first;
    //*row.second++ = scalar1;
    //*row.first++ = *colPair.second;
    //*row.second++ = scalar2;

    it = ++it2;
  }
  //matrix.rowDone();
}

void F4MatrixBuilder2::appendRowSPair(
  const Poly* poly,
  monomial multiply,
  const Poly* sPairPoly,
  monomial sPairMultiply,
  F4PreBlock& block,
  TaskFeeder& feeder
) {
  MATHICGB_ASSERT(!poly->isZero());
  MATHICGB_ASSERT(!multiply.isNull());
  MATHICGB_ASSERT(ring().hashValid(multiply));
  MATHICGB_ASSERT(sPairPoly != 0);
  Poly::const_iterator itA = poly->begin();
  const Poly::const_iterator endA = poly->end();

  MATHICGB_ASSERT(!sPairPoly->isZero());
  MATHICGB_ASSERT(!sPairMultiply.isNull());
  MATHICGB_ASSERT(ring().hashValid(sPairMultiply));
  Poly::const_iterator itB = sPairPoly->begin();
  Poly::const_iterator endB = sPairPoly->end();

  // skip leading terms since they cancel
  MATHICGB_ASSERT(itA.getCoefficient() == itB.getCoefficient());
  ++itA;
  ++itB;

  // @todo: handle overflow of termCount addition here
  MATHICGB_ASSERT(poly->termCount() + sPairPoly->termCount() - 2 <=
    std::numeric_limits<ColIndex>::max());
  const auto maxCols =
    static_cast<ColIndex>(poly->termCount() + sPairPoly->termCount() - 2);
  auto row = block.makeRow(maxCols);
  const auto indicesBegin = row.first;

  const ColReader colMap(mMap);

  const const_monomial mulA = multiply;
  const const_monomial mulB = sPairMultiply;
  while (itB != endB && itA != endA) {
    const auto colA = findOrCreateColumn
      (itA.getMonomial(), mulA, colMap, feeder);
    const auto colB = findOrCreateColumn
      (itB.getMonomial(), mulB, colMap, feeder);
    const auto cmp = ring().monomialCompare(colA.second, colB.second);

    coefficient coeff = 0;
    ColIndex col;
    if (cmp != LT) {
      coeff = itA.getCoefficient();
      col = colA.first;
      ++itA;
    }
    if (cmp != GT) {
      coeff = ring().coefficientSubtract(coeff, itB.getCoefficient());
      col = colB.first;
      ++itB;
    }
    MATHICGB_ASSERT(coeff < std::numeric_limits<Scalar>::max());
    if (coeff != 0) {
      //matrix.appendEntry(col, static_cast<Scalar>(coeff));
      *row.first++ = col;
      *row.second++ = static_cast<Scalar>(coeff);
    }
  }

  for (; itA != endA; ++itA) {
    const auto colA = findOrCreateColumn
      (itA.getMonomial(), mulA, colMap, feeder);
    //matrix.appendEntry(colA.first, static_cast<Scalar>(itA.getCoefficient()));
    *row.first++ = colA.first;
    *row.second++ = static_cast<Scalar>(itA.getCoefficient());
  }

  for (; itB != endB; ++itB) {
    const auto colB = findOrCreateColumn
      (itB.getMonomial(), mulB, colMap, feeder);
    const auto negative = ring().coefficientNegate(itB.getCoefficient());
    //matrix.appendEntry(colB.first, static_cast<Scalar>(negative));
    *row.first++ = colB.first;
    *row.second++ = static_cast<Scalar>(negative);
  }

  const auto toRemove =
    maxCols - static_cast<ColIndex>(row.first - indicesBegin);
  block.removeLastEntries(block.rowCount() - 1, toRemove);
}
