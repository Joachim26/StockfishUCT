/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <cassert>

#include "bitboard.h"
#include "bitcount.h"
#include "endgame.h"
#include "movegen.h"

using std::string;

namespace {

  // Table used to drive the king towards the edge of the board
  // in KX vs K and KQ vs KR endgames.
  const int PushToEdges[SQUARE_NB] = {
    400, 360, 320, 280, 280, 320, 360, 400,
    360, 280, 240, 200, 200, 240, 280, 360,
    320, 240, 160, 120, 120, 160, 240, 320,
    280, 200, 120,  80,  80, 120, 200, 280,
    280, 200, 120,  80,  80, 120, 200, 280,
    320, 240, 160, 120, 120, 160, 240, 320,
    360, 280, 240, 200, 200, 240, 280, 360,
    400, 360, 320, 280, 280, 320, 360, 400,
  };

  // Table used to drive the king towards a corner square of the
  // right color in KBN vs K endgames.
  const int PushToCorners[SQUARE_NB] = {
    800, 700, 600, 500, 400, 300, 200, 100,
    700, 560, 460, 360, 260, 160,  60, 200,
    600, 460, 320, 220, 120,  20, 160, 300,
    500, 360, 220,  50, -50, 120, 260, 400,
    400, 260, 120, -50,  50, 220, 360, 500,
    300, 160,  20, 120, 220, 320, 460, 600,
    200,  60, 160, 260, 360, 460, 560, 700,
    100, 200, 300, 400, 500, 600, 700, 800
  };

  // Tables used to drive a piece towards or away from another piece
  const int PushClose[8] = { 0, 0, 400, 320, 240, 160, 80, 40 };
  const int PushAway [8] = { 0, 20, 80, 160, 240, 320, 360, 400 };

  // FortressMask[Color] used by KQ vs KR and one or more pawns endgame
  Bitboard FortressMask[] = {
      0x00007E4242C37E00ULL,
      0x007EC342427E0000ULL
  };

  // Pawn Rank based scaling factors used in KRPPKRP endgame
  const int KRPPKRPScaleFactors[RANK_NB] = { 0, 9, 10, 14, 21, 44, 0, 0 };

#ifndef NDEBUG
  bool verify_material(const Position& pos, Color c, Value npm, int pawnsCnt) {
    return pos.non_pawn_material(c) == npm && pos.count<PAWN>(c) == pawnsCnt;
  }
#endif

  // Map the square as if strongSide is white and strongSide's only pawn
  // is on the left half of the board.
  Square normalize(const Position& pos, Color strongSide, Square sq) {

    assert(pos.count<PAWN>(strongSide) == 1);

    if (file_of(pos.square<PAWN>(strongSide)) >= FILE_E)
        sq = Square(sq ^ 7); // Mirror SQ_H1 -> SQ_A1

    if (strongSide == BLACK)
        sq = ~sq;

    return sq;
  }

  // Get the material key of Position out of the given endgame key code
  // like "KBPKN". The trick here is to first forge an ad-hoc FEN string
  // and then let a Position object do the work for us.
  Key key(const string& code, Color c) {

    assert(code.length() > 0 && code.length() < 8);
    assert(code[0] == 'K');

    string sides[] = { code.substr(code.find('K', 1)),      // Weak
                       code.substr(0, code.find('K', 1)) }; // Strong

    std::transform(sides[c].begin(), sides[c].end(), sides[c].begin(), tolower);

    string fen =  sides[0] + char(8 - sides[0].length() + '0') + "/8/8/8/8/8/8/"
                + sides[1] + char(8 - sides[1].length() + '0') + " w - - 0 10";

    return Position(fen, false, nullptr).material_key();
  }

} // namespace


/// Endgames members definitions

Endgames::Endgames() {

  add<KPK>("KPK");
  add<KNNK>("KNNK");
  add<KRKP>("KRKP");
  add<KQKP>("KQKP");

  add<KNPK>("KNPK");
  add<KNPKB>("KNPKB");
  add<KRPKR>("KRPKR");
  add<KRPKB>("KRPKB");
  add<KBPKB>("KBPKB");
  add<KBPKN>("KBPKN");
  add<KBPPKB>("KBPPKB");
  add<KRPPKRP>("KRPPKRP");
}


template<EndgameType E, typename T>
void Endgames::add(const string& code) {
  map<T>()[key(code, WHITE)] = std::unique_ptr<EndgameBase<T>>(new Endgame<E>(WHITE));
  map<T>()[key(code, BLACK)] = std::unique_ptr<EndgameBase<T>>(new Endgame<E>(BLACK));
}


/// Mate with KX vs K. This function is used to evaluate positions with king and
/// plenty of material vs a lone king. It simply gives the attacking side
/// a bonus for driving the defending king towards the edge of the board,
/// for keeping the distance between the two kings small, and, in case of KBNK,
/// to drive the defending king towards a corner square of the right color.
template<>
Value Endgame<KXK>::operator()(const Position& pos) const {

  assert(verify_material(pos, weakSide, VALUE_ZERO, 0));
  assert(!pos.checkers()); // Eval is never called when in check

  // Stalemate detection with lone king
  if (pos.side_to_move() == weakSide && !MoveList<LEGAL>(pos).size())
      return VALUE_DRAW;

  // Draw detection with 2 or more bishops of the same color (and no pawns!)
  if (    pos.count<BISHOP>(strongSide) > 1
      && !(   pos.pieces(strongSide, BISHOP) &  DarkSquares
           && pos.pieces(strongSide, BISHOP) & ~DarkSquares)
      && !pos.count<PAWN  >(strongSide)  // to
      && !pos.count<KNIGHT>(strongSide)  // avoid
      && !pos.count<ROOK  >(strongSide)  // false
      && !pos.count<QUEEN >(strongSide)) // positives!
      return VALUE_DRAW;

  Square winnerKSq = pos.square<KING>(strongSide);
  Square loserKSq = pos.square<KING>(weakSide);

  Value result =  VALUE_KNOWN_WIN
                + pos.non_pawn_material(strongSide) / 10
                + PushToEdges[loserKSq]
                + PushClose[distance(winnerKSq, loserKSq)];

  if (   pos.count<BISHOP>(strongSide) == 1
      && pos.count<KNIGHT>(strongSide) == 1)
  {
      Square bishopSq = pos.square<BISHOP>(strongSide);

      if (opposite_colors(bishopSq, SQ_A1))
      {
          winnerKSq = ~winnerKSq;
          loserKSq  = ~loserKSq;
      }

      result += PushToCorners[loserKSq];
  }

  return strongSide == pos.side_to_move() ? result : -result;
}


/// KP vs K. This endgame is evaluated with the help of a bitbase.
template<>
Value Endgame<KPK>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, VALUE_ZERO, 1));
  assert(verify_material(pos, weakSide, VALUE_ZERO, 0));

  // Assume strongSide is white and the pawn is on files A-D
  Square wksq = normalize(pos, strongSide, pos.square<KING>(strongSide));
  Square bksq = normalize(pos, strongSide, pos.square<KING>(weakSide));
  Square psq  = normalize(pos, strongSide, pos.square<PAWN>(strongSide));

  Color us = strongSide == pos.side_to_move() ? WHITE : BLACK;

  if (!Bitbases::probe(wksq, psq, bksq, us))
      return VALUE_DRAW;

  Value result = VALUE_KNOWN_WIN - PawnValueEg / 4 * (7 - rank_of(psq));

  return strongSide == pos.side_to_move() ? result : -result;
}


/// KR vs KP. This is a somewhat tricky endgame to evaluate precisely without
/// a bitbase. The function below returns drawish scores when the pawn is
/// far advanced with support of the king, while the attacking king is far
/// away.
template<>
Value Endgame<KRKP>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, RookValueMg, 0));
  assert(verify_material(pos, weakSide, VALUE_ZERO, 1));

  Square wksq = relative_square(strongSide, pos.square<KING>(strongSide));
  Square bksq = relative_square(strongSide, pos.square<KING>(weakSide));
  Square rsq  = relative_square(strongSide, pos.square<ROOK>(strongSide));
  Square psq  = relative_square(strongSide, pos.square<PAWN>(weakSide));

  Square queeningSq = make_square(file_of(psq), RANK_1);
  Value result;

  // If both, the pawn and the king of the weaker side, are not beyond
  // the 3rd rank and it's the stronger side to move, it's a win.
  if (   rank_of(bksq) >= RANK_6
      && rank_of(psq ) >= RANK_6
      && pos.side_to_move() == strongSide)
      result = VALUE_KNOWN_WIN + RookValueEg / 10 - PawnValueEg;

  // If the stronger side's king is in front of the pawn, it's a win
  else if (    wksq < psq
           && distance<File>(wksq, psq) <= 1
           && (rank_of(psq) >= RANK_3 || distance(bksq, psq) >= 2))
      result = VALUE_KNOWN_WIN + RookValueEg / 10 - PawnValueEg;

  // If the weaker side's king is too far from the pawn and the rook,
  // it's a win.
  else if (    distance(bksq, psq) >= 3 + (pos.side_to_move() == weakSide)
           &&  distance(bksq, rsq) >= 2
           && (rank_of(psq) != RANK_2 || distance(wksq, queeningSq) <= 1))
      result = VALUE_KNOWN_WIN + RookValueEg / 10 - PawnValueEg;

  // If the pawn is far advanced and supported by the defending king,
  // the position is drawish
  else if (   rank_of(bksq) <= RANK_3
           && distance(bksq, psq) == 1
           && rank_of(wksq) >= RANK_4
           && distance(wksq, psq) > 2 + (pos.side_to_move() == strongSide))
      result = Value(80) - 8 * distance(wksq, psq);

  else
      result =  Value(200) - 8 * (  distance(wksq, psq + DELTA_S)
                                  - distance(bksq, psq + DELTA_S)
                                  - distance(psq, queeningSq));

  return strongSide == pos.side_to_move() ? result : -result;
}


/// KQ vs KP. In general, this is a win for the stronger side, but there are a
/// few important exceptions. A pawn on 7th rank and on the A,C,F or H files
/// with a king positioned next to it can be a draw, so in that case, we only
/// use the distance between the kings.
template<>
Value Endgame<KQKP>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, QueenValueMg, 0));
  assert(verify_material(pos, weakSide, VALUE_ZERO, 1));

  Square winnerKSq = pos.square<KING>(strongSide);
  Square loserKSq = pos.square<KING>(weakSide);
  Square pawnSq = pos.square<PAWN>(weakSide);

  Value result = Value(PushClose[distance(winnerKSq, loserKSq)] / (pos.rule50_count() + 1));

  if (   relative_rank(weakSide, pawnSq) != RANK_7
      || distance(loserKSq, pawnSq) != 1
      || !((FileABB | FileCBB | FileFBB | FileHBB) & pawnSq))
      result += VALUE_KNOWN_WIN + QueenValueEg / 10 - PawnValueEg;

  return strongSide == pos.side_to_move() ? result : -result;
}


/// Some cases of trivial draws
template<> Value Endgame<KNNK>::operator()(const Position&) const { return VALUE_DRAW; }


/// KB and one or more pawns vs K. It checks for draws with rook pawns and
/// a bishop of the wrong color. If such a draw is detected, SCALE_FACTOR_DRAW
/// is returned. If not, the return value is SCALE_FACTOR_NONE, i.e. no scaling
/// will be used.
template<>
ScaleFactor Endgame<KBPsK>::operator()(const Position& pos) const {

  assert(pos.non_pawn_material(strongSide) == BishopValueMg);
  assert(pos.count<PAWN>(strongSide) >= 1);

  // No assertions about the material of weakSide, because we want draws to
  // be detected even when the weaker side has some pawns.

  Bitboard pawns = pos.pieces(strongSide, PAWN);
  File pawnsFile = file_of(lsb(pawns));

  // All pawns are on a single rook file?
  if (    (pawnsFile == FILE_A || pawnsFile == FILE_H)
      && !(pawns & ~file_bb(pawnsFile)))
  {
      Square bishopSq = pos.square<BISHOP>(strongSide);
      Square queeningSq = relative_square(strongSide, make_square(pawnsFile, RANK_8));
      Square kingSq = pos.square<KING>(weakSide);

      if (   opposite_colors(queeningSq, bishopSq)
          && distance(queeningSq, kingSq) <= 1)
          return SCALE_FACTOR_DRAW;
  }

  // Check for the fortress draw in KBPK
  if (    pos.count<PAWN>(strongSide) == 1
      && !more_than_one(pos.pieces(weakSide))
      && (pawnsFile == FILE_B || pawnsFile == FILE_G))
  {
      // Assume strongSide is white and the pawn is on files A-D
      Square pawnSq     = normalize(pos, strongSide, pos.square<PAWN>(strongSide));
      Square weakKingSq = normalize(pos, strongSide, pos.square<KING>(weakSide));
      Square bishopSq   = normalize(pos, strongSide, pos.square<BISHOP>(strongSide));

      if (pawnSq == SQ_B6 && bishopSq == SQ_A7 && (weakKingSq == SQ_B7 || weakKingSq == SQ_A8))
          return SCALE_FACTOR_DRAW;
  }

  // If all the pawns are on the same B or G file, then it's potentially a draw
  if (    (pawnsFile == FILE_B || pawnsFile == FILE_G)
      && !(pos.pieces(PAWN) & ~file_bb(pawnsFile))
      && pos.non_pawn_material(weakSide) == 0
      && pos.count<PAWN>(weakSide) >= 1)
  {
      // Get weakSide pawn that is closest to the home rank
      Square weakPawnSq = backmost_sq(weakSide, pos.pieces(weakSide, PAWN));

      Square strongKingSq = pos.square<KING>(strongSide);
      Square weakKingSq = pos.square<KING>(weakSide);
      Square bishopSq = pos.square<BISHOP>(strongSide);

      // There's potential for a draw if our pawn is blocked on the 7th rank,
      // the bishop cannot attack it or they only have one pawn left
      if (   relative_rank(strongSide, weakPawnSq) == RANK_7
          && (pos.pieces(strongSide, PAWN) & (weakPawnSq + pawn_push(weakSide)))
          && (opposite_colors(bishopSq, weakPawnSq) || pos.count<PAWN>(strongSide) == 1))
      {
          int strongKingDist = distance(weakPawnSq, strongKingSq);
          int weakKingDist = distance(weakPawnSq, weakKingSq);

          // It's a draw if the weak king is on its back two ranks, within 2
          // squares of the blocking pawn and the strong king is not
          // closer. (I think this rule only fails in practically
          // unreachable positions such as 5k1K/6p1/6P1/8/8/3B4/8/8 w
          // and positions where qsearch will immediately correct the
          // problem such as 8/4k1p1/6P1/1K6/3B4/8/8/8 w)
          if (   relative_rank(strongSide, weakKingSq) >= RANK_7
              && weakKingDist <= 2
              && weakKingDist <= strongKingDist)
              return SCALE_FACTOR_DRAW;
      }
  }

  return SCALE_FACTOR_NONE;
}


/// KQ vs KR and one or more pawns. It tests for fortress draws with a rook on
/// the third rank defended by a pawn.
template<>
ScaleFactor Endgame<KQKRPs>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, QueenValueMg, 0));
  assert(pos.count<ROOK>(weakSide) == 1);
  assert(pos.count<PAWN>(weakSide) >= 1);

  Square strongKingSq = pos.square<KING>(strongSide);
  Square weakKingSq = pos.square<KING>(weakSide);
  Square rsq = pos.square<ROOK>(weakSide);

  if (      pos.pieces(weakSide, PAWN) & FortressMask[weakSide]
      &&    relative_rank(weakSide, strongKingSq) > relative_rank(weakSide, rsq)
      && (  pos.pieces(weakSide, PAWN)
          & pos.attacks_from<KING>(weakKingSq)
          & pos.attacks_from<PAWN>(rsq, strongSide)))
          return SCALE_FACTOR_DRAW;

  return pos.rule50_count() > 14 ? ScaleFactor(int(SCALE_FACTOR_NORMAL * double((101 - pos.rule50_count()) / 172)))
                                 : SCALE_FACTOR_NONE;
}


/// KRP vs KR. This function knows a handful of the most important classes of
/// drawn positions, but is far from perfect. It would probably be a good idea
/// to add more knowledge in the future.
///
/// It would also be nice to rewrite the actual code for this function,
/// which is mostly copied from Glaurung 1.x, and isn't very pretty.
template<>
ScaleFactor Endgame<KRPKR>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, RookValueMg, 1));
  assert(verify_material(pos, weakSide,   RookValueMg, 0));

  // Assume strongSide is white and the pawn is on files A-D
  Square wksq = normalize(pos, strongSide, pos.square<KING>(strongSide));
  Square bksq = normalize(pos, strongSide, pos.square<KING>(weakSide));
  Square wrsq = normalize(pos, strongSide, pos.square<ROOK>(strongSide));
  Square wpsq = normalize(pos, strongSide, pos.square<PAWN>(strongSide));
  Square brsq = normalize(pos, strongSide, pos.square<ROOK>(weakSide));

  File f = file_of(wpsq);
  Rank r = rank_of(wpsq);
  Square queeningSq = make_square(f, RANK_8);
  int tempo = (pos.side_to_move() == strongSide);

  // If the pawn is not too far advanced and the defending king defends the
  // queening square, use the third-rank defence.
  if (   r <= RANK_5
      && distance(bksq, queeningSq) <= 1
      && wksq <= SQ_H5
      && (rank_of(brsq) == RANK_6 || (r <= RANK_3 && rank_of(wrsq) != RANK_6)))
      return SCALE_FACTOR_DRAW;

  // The defending side saves a draw by checking from behind in case the pawn
  // has advanced to the 6th rank with the king behind.
  if (   r == RANK_6
      && distance(bksq, queeningSq) <= 1
      && rank_of(wksq) + tempo <= RANK_6
      && (rank_of(brsq) == RANK_1 || (!tempo && distance<File>(brsq, wpsq) >= 3)))
      return SCALE_FACTOR_DRAW;

  if (   r >= RANK_6
      && bksq == queeningSq
      && rank_of(brsq) == RANK_1
      && (!tempo || distance(wksq, wpsq) >= 2))
      return SCALE_FACTOR_DRAW;

  // White pawn on a7 and rook on a8 is a draw if black's king is on g7 or h7
  // and the black rook is behind the pawn.
  if (   wpsq == SQ_A7
      && wrsq == SQ_A8
      && (bksq == SQ_H7 || bksq == SQ_G7)
      && file_of(brsq) == FILE_A
      && (rank_of(brsq) <= RANK_3 || file_of(wksq) >= FILE_D || rank_of(wksq) <= RANK_5))
      return SCALE_FACTOR_DRAW;

  // If the defending king blocks the pawn and the attacking king is too far
  // away, it's a draw.
  if (   r <= RANK_5
      && bksq == wpsq + DELTA_N
      && distance(wksq, wpsq) - tempo >= 2
      && distance(wksq, brsq) - tempo >= 2)
      return SCALE_FACTOR_DRAW;

  // Pawn on the 7th rank supported by the rook from behind usually wins if the
  // attacking king is closer to the queening square than the defending king,
  // and the defending king cannot gain tempi by threatening the attacking rook.
  if (   r == RANK_7
      && f != FILE_A
      && file_of(wrsq) == f
      && wrsq != queeningSq
      && (distance(wksq, queeningSq) < distance(bksq, queeningSq) - 2 + tempo)
      && (distance(wksq, queeningSq) < distance(bksq, wrsq) + tempo))
      return ScaleFactor(SCALE_FACTOR_MAX - 2 * distance(wksq, queeningSq));

  // Similar to the above, but with the pawn further back
  if (   f != FILE_A
      && file_of(wrsq) == f
      && wrsq < wpsq
      && (distance(wksq, queeningSq) < distance(bksq, queeningSq) - 2 + tempo)
      && (distance(wksq, wpsq + DELTA_N) < distance(bksq, wpsq + DELTA_N) - 2 + tempo)
      && (  distance(bksq, wrsq) + tempo >= 3
          || (    distance(wksq, queeningSq) < distance(bksq, wrsq) + tempo
              && (distance(wksq, wpsq + DELTA_N) < distance(bksq, wrsq) + tempo))))
      return ScaleFactor(  SCALE_FACTOR_MAX
                         - 8 * distance(wpsq, queeningSq)
                         - 2 * distance(wksq, queeningSq));

  // If the pawn is not far advanced and the defending king is somewhere in
  // the pawn's path, it's probably a draw.
  if (r <= RANK_4 && bksq > wpsq)
  {
      if (file_of(bksq) == file_of(wpsq))
          return ScaleFactor(10);
      if (   distance<File>(bksq, wpsq) == 1
          && distance(wksq, bksq) > 2)
          return ScaleFactor(24 - 2 * distance(wksq, bksq));
  }
  return SCALE_FACTOR_NONE;
}

template<>
ScaleFactor Endgame<KRPKB>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, RookValueMg, 1));
  assert(verify_material(pos, weakSide, BishopValueMg, 0));

  // Test for a rook pawn
  if (pos.pieces(PAWN) & (FileABB | FileHBB))
  {
      Square ksq = pos.square<KING>(weakSide);
      Square bsq = pos.square<BISHOP>(weakSide);
      Square psq = pos.square<PAWN>(strongSide);
      Rank rk = relative_rank(strongSide, psq);
      Square push = pawn_push(strongSide);

      // If the pawn is on the 5th rank and the pawn (currently) is on
      // the same color square as the bishop then there is a chance of
      // a fortress. Depending on the king position give a moderate
      // reduction or a stronger one if the defending king is near the
      // corner but not trapped there.
      if (rk == RANK_5 && !opposite_colors(bsq, psq))
      {
          int d = distance(psq + 3 * push, ksq);

          if (d <= 2 && !(d == 0 && ksq == pos.square<KING>(strongSide) + 2 * push))
              return ScaleFactor(24);
          else
              return ScaleFactor(48);
      }

      // When the pawn has moved to the 6th rank we can be fairly sure
      // it's drawn if the bishop attacks the square in front of the
      // pawn from a reasonable distance and the defending king is near
      // the corner
      if (   rk == RANK_6
          && distance(psq + 2 * push, ksq) <= 1
          && (PseudoAttacks[BISHOP][bsq] & (psq + push))
          && distance<File>(bsq, psq) >= 2)
          return ScaleFactor(8);
  }

  return SCALE_FACTOR_NONE;
}

/// KRPP vs KRP. There is just a single rule: if the stronger side has no passed
/// pawns and the defending king is actively placed, the position is drawish.
template<>
ScaleFactor Endgame<KRPPKRP>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, RookValueMg, 2));
  assert(verify_material(pos, weakSide,   RookValueMg, 1));

  Square wpsq1 = pos.squares<PAWN>(strongSide)[0];
  Square wpsq2 = pos.squares<PAWN>(strongSide)[1];
  Square bksq = pos.square<KING>(weakSide);

  // Does the stronger side have a passed pawn?
  if (pos.pawn_passed(strongSide, wpsq1) || pos.pawn_passed(strongSide, wpsq2))
      return SCALE_FACTOR_NONE;

  Rank r = std::max(relative_rank(strongSide, wpsq1), relative_rank(strongSide, wpsq2));

  if (   distance<File>(bksq, wpsq1) <= 1
      && distance<File>(bksq, wpsq2) <= 1
      && relative_rank(strongSide, bksq) > r)
  {
      assert(r > RANK_1 && r < RANK_7);
      return ScaleFactor(KRPPKRPScaleFactors[r]);
  }
  return SCALE_FACTOR_NONE;
}


/// K and two or more pawns vs K. There is just a single rule here: If all pawns
/// are on the same rook file and are blocked by the defending king, it's a draw.
template<>
ScaleFactor Endgame<KPsK>::operator()(const Position& pos) const {

  assert(pos.non_pawn_material(strongSide) == VALUE_ZERO);
  assert(pos.count<PAWN>(strongSide) >= 2);
  assert(verify_material(pos, weakSide, VALUE_ZERO, 0));

  Square ksq = pos.square<KING>(weakSide);
  Bitboard pawns = pos.pieces(strongSide, PAWN);

  // If all pawns are ahead of the king, on a single rook file and
  // the king is within one file of the pawns, it's a draw.
  if (   !(pawns & ~in_front_bb(weakSide, rank_of(ksq)))
      && !((pawns & ~FileABB) && (pawns & ~FileHBB))
      &&  distance<File>(ksq, lsb(pawns)) <= 1)
      return SCALE_FACTOR_DRAW;

  return SCALE_FACTOR_NONE;
}


/// KBP vs KB. There are two rules: if the defending king is somewhere along the
/// path of the pawn, and the square of the king is not of the same color as the
/// stronger side's bishop, it's a draw. If the two bishops have opposite color,
/// it's almost always a draw.
template<>
ScaleFactor Endgame<KBPKB>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, BishopValueMg, 1));
  assert(verify_material(pos, weakSide,   BishopValueMg, 0));

  Square pawnSq = pos.square<PAWN>(strongSide);
  Square strongBishopSq = pos.square<BISHOP>(strongSide);
  Square weakBishopSq = pos.square<BISHOP>(weakSide);
  Square weakKingSq = pos.square<KING>(weakSide);

  // Case 1: Defending king blocks the pawn, and cannot be driven away
  if (   file_of(weakKingSq) == file_of(pawnSq)
      && relative_rank(strongSide, pawnSq) < relative_rank(strongSide, weakKingSq)
      && (   opposite_colors(weakKingSq, strongBishopSq)
          || relative_rank(strongSide, weakKingSq) <= RANK_6))
      return SCALE_FACTOR_DRAW;

  // Case 2: Opposite colored bishops
  if (opposite_colors(strongBishopSq, weakBishopSq))
  {
      // We assume that the position is drawn in the following three situations:
      //
      //   a. The pawn is on rank 5 or further back.
      //   b. The defending king is somewhere in the pawn's path.
      //   c. The defending bishop attacks some square along the pawn's path,
      //      and is at least three squares away from the pawn.
      //
      // These rules are probably not perfect, but in practice they work
      // reasonably well.

      if (relative_rank(strongSide, pawnSq) <= RANK_5)
          return SCALE_FACTOR_DRAW;
      else
      {
          Bitboard path = forward_bb(strongSide, pawnSq);

          if (path & pos.pieces(weakSide, KING))
              return SCALE_FACTOR_DRAW;

          if (  (pos.attacks_from<BISHOP>(weakBishopSq) & path)
              && distance(weakBishopSq, pawnSq) >= 3)
              return SCALE_FACTOR_DRAW;
      }
  }
  return SCALE_FACTOR_NONE;
}


/// KBPP vs KB. It detects a few basic draws with opposite-colored bishops
template<>
ScaleFactor Endgame<KBPPKB>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, BishopValueMg, 2));
  assert(verify_material(pos, weakSide,   BishopValueMg, 0));

  Square wbsq = pos.square<BISHOP>(strongSide);
  Square bbsq = pos.square<BISHOP>(weakSide);

  if (!opposite_colors(wbsq, bbsq))
      return SCALE_FACTOR_NONE;

  Square ksq = pos.square<KING>(weakSide);
  Square psq1 = pos.squares<PAWN>(strongSide)[0];
  Square psq2 = pos.squares<PAWN>(strongSide)[1];
  Rank r1 = rank_of(psq1);
  Rank r2 = rank_of(psq2);
  Square blockSq1, blockSq2;

  if (relative_rank(strongSide, psq1) > relative_rank(strongSide, psq2))
  {
      blockSq1 = psq1 + pawn_push(strongSide);
      blockSq2 = make_square(file_of(psq2), rank_of(psq1));
  }
  else
  {
      blockSq1 = psq2 + pawn_push(strongSide);
      blockSq2 = make_square(file_of(psq1), rank_of(psq2));
  }

  switch (distance<File>(psq1, psq2))
  {
  case 0:
    // Both pawns are on the same file. It's an easy draw if the defender firmly
    // controls some square in the frontmost pawn's path.
    if (   file_of(ksq) == file_of(blockSq1)
        && relative_rank(strongSide, ksq) >= relative_rank(strongSide, blockSq1)
        && opposite_colors(ksq, wbsq))
        return SCALE_FACTOR_DRAW;
    else
        return SCALE_FACTOR_NONE;

  case 1:
    // Pawns on adjacent files. It's a draw if the defender firmly controls the
    // square in front of the frontmost pawn's path, and the square diagonally
    // behind this square on the file of the other pawn.
    if (   ksq == blockSq1
        && opposite_colors(ksq, wbsq)
        && (   bbsq == blockSq2
            || (pos.attacks_from<BISHOP>(blockSq2) & pos.pieces(weakSide, BISHOP))
            || distance(r1, r2) >= 2))
        return SCALE_FACTOR_DRAW;

    else if (   ksq == blockSq2
             && opposite_colors(ksq, wbsq)
             && (   bbsq == blockSq1
                 || (pos.attacks_from<BISHOP>(blockSq1) & pos.pieces(weakSide, BISHOP))))
        return SCALE_FACTOR_DRAW;
    else
        return SCALE_FACTOR_NONE;

  default:
    // The pawns are not on the same file or adjacent files. No scaling.
    return SCALE_FACTOR_NONE;
  }
}


/// KBP vs KN. There is a single rule: If the defending king is somewhere along
/// the path of the pawn, and the square of the king is not of the same color as
/// the stronger side's bishop, it's a draw.
template<>
ScaleFactor Endgame<KBPKN>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, BishopValueMg, 1));
  assert(verify_material(pos, weakSide, KnightValueMg, 0));

  Square pawnSq = pos.square<PAWN>(strongSide);
  Square strongBishopSq = pos.square<BISHOP>(strongSide);
  Square weakKingSq = pos.square<KING>(weakSide);

  if (   file_of(weakKingSq) == file_of(pawnSq)
      && relative_rank(strongSide, pawnSq) < relative_rank(strongSide, weakKingSq)
      && (   opposite_colors(weakKingSq, strongBishopSq)
          || relative_rank(strongSide, weakKingSq) <= RANK_6))
      return SCALE_FACTOR_DRAW;

  return SCALE_FACTOR_NONE;
}


/// KNP vs K. There is a single rule: if the pawn is a rook pawn on the 7th rank
/// and the defending king prevents the pawn from advancing, the position is drawn.
template<>
ScaleFactor Endgame<KNPK>::operator()(const Position& pos) const {

  assert(verify_material(pos, strongSide, KnightValueMg, 1));
  assert(verify_material(pos, weakSide, VALUE_ZERO, 0));

  // Assume strongSide is white and the pawn is on files A-D
  Square pawnSq       = normalize(pos, strongSide, pos.square<PAWN>(strongSide));
  Square knightSq     = normalize(pos, strongSide, pos.square<KNIGHT>(strongSide));
  Square strongKingSq = normalize(pos, strongSide, pos.square<KING>(strongSide));
  Square weakKingSq   = normalize(pos, strongSide, pos.square<KING>(weakSide));

  if (pawnSq == SQ_A7)
  {
      if (weakKingSq == SQ_A8 || weakKingSq == SQ_B7)
          return SCALE_FACTOR_DRAW;

      else if ((weakKingSq == SQ_C8 || weakKingSq == SQ_C7)
          &&  strongKingSq == SQ_A8
          && (strongSide == pos.side_to_move()) == !opposite_colors(weakKingSq, knightSq))
          return SCALE_FACTOR_DRAW;
  }
  return SCALE_FACTOR_NONE;
}


/// KNP vs KB. If knight can block bishop from taking pawn, it's a win.
/// Otherwise the position is drawn.
template<>
ScaleFactor Endgame<KNPKB>::operator()(const Position& pos) const {

  Square pawnSq = pos.square<PAWN>(strongSide);
  Square bishopSq = pos.square<BISHOP>(weakSide);
  Square weakKingSq = pos.square<KING>(weakSide);

  // King needs to get close to promoting pawn to prevent knight from blocking.
  // Rules for this are very tricky, so just approximate.
  if (forward_bb(strongSide, pawnSq) & pos.attacks_from<BISHOP>(bishopSq))
      return ScaleFactor(distance(weakKingSq, pawnSq));

  return SCALE_FACTOR_NONE;
}
