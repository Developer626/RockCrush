#include "mainWindow.h"
#include "globals.h"
#include "persistence.h"
#include "main.h"

//

static GRect s_wave[3];
static int s_waveV = 0;
static GRect s_liquid;
static GColor s_colourBackground;
static GColor s_colourForground;
static int s_nWaves = 0;
static int s_liquidEnd = 0;
static int s_windowSizeY = 0;

static bool s_autoMode = 0;
static int s_frame = 0;

static AppTimer* s_hintTimer = NULL;
static AppTimer* s_gameLoopTime = NULL;

static Layer* s_mainWindowLayer = NULL;
static Layer* s_boardLayer = NULL;
static StatusBarLayer* s_statusBar = NULL;

static GPoint s_cursor = {0,0};
static GPoint s_motionCursor = {400,400};
static GPoint s_availableMove = {-1,-1};

//static GPoint s_bombLocation[N_COLOURS];

static Score_t s_score;
static Piece_t s_pieces[BOARD_PIECES];
static Switch_t s_switch;
static int s_currentRun;

static GameState_t s_gameState = kIdle; // Game FSM
static ScoreState_t s_scoreState = kWait;


#define N_LEVEL_COLOURS 13
static GColor s_levelColour[N_LEVEL_COLOURS];

Score_t* getScore() { return &s_score; };
Piece_t* getPiece() { return &s_pieces[0]; }
int XY(int x, int y) { return x + (BOARD_PIECES_X * y); }
int XYp(GPoint p) { return XY(p.x,p.y); }

static void redraw() {
  layer_mark_dirty(s_mainWindowLayer);
};

// One point per square in a match 3
#define SCORE_3 3
// Two points per square in a match 4
#define SCORE_4 6
//Note this is bonus on top of two x SCORE_3's
#define SCORE_T 3
// Two points per square clearing a row/col
#define SCORE_ROW BOARD_PIECES_X
#define SCORE_COLUMN BOARD_PIECES_Y
// Two points _per exploded piece_
#define SCORE_BOOM 2
void score(MatchType_t type, int n) {
  int b4 = s_score.pointBuffer;
  int M = (s_score.level + 1) / 2; // Bonuses are worth more in later levels
  switch (type) {
    case kRow: s_score.pointBuffer += SCORE_COLUMN * M; break;
    case kColumn: s_score.pointBuffer += SCORE_ROW * M; break;
    case kCross: s_score.pointBuffer += (SCORE_ROW+SCORE_COLUMN) * M; break;
    case kBOOM: case kColourBoom: case kMiniBoom: s_score.pointBuffer += SCORE_BOOM * n * M; break;
    case kMatch3: s_score.pointBuffer += SCORE_3 + M; break; // Note - plus - nerfed
    case kMatch4: s_score.pointBuffer += SCORE_4 + M; break; // Note - plus - nerfed
    case kMatchT: s_score.pointBuffer += SCORE_T * M; break;
  }
  APP_LOG(APP_LOG_LEVEL_INFO, "points enum %i scored %i", type, s_score.pointBuffer - b4);
}

bool checkTriplet(Colour_t a, Colour_t b, Colour_t switchColour) {
    if (a == kNONE || b == kNONE || switchColour == kNONE) return false;
    if (switchColour == kBlack) { APP_LOG(APP_LOG_LEVEL_DEBUG, "Y-black"); return true; }// Always valid

    int nWhite = 0;
    if (a == kWhite) ++nWhite;
    if (b == kWhite) ++nWhite;
    if (switchColour == kWhite) ++nWhite;

    if (nWhite > 1) {
      return true; //must be WcW for WWc or cWW will auto-fire
    } else if (nWhite == 1) {
      if (a == b || b == switchColour || switchColour == a) return true; // White fills the gap
    } else {
      if (a == b && b  == switchColour) return true;  // No white, all three must match
    }

    return false;
}

void switchColours(GPoint a, GPoint b) {
  Colour_t temp = s_pieces[ XYp(a) ].colour;
  s_pieces[ XYp(a) ].colour = s_pieces[ XYp(b) ].colour;
  s_pieces[ XYp(b) ].colour = temp;
}

void switchPieces() {
  Piece_t temp = s_pieces[ XYp(s_switch.first) ];
  s_pieces[ XYp(s_switch.first) ] = s_pieces[ XYp(s_switch.second) ];
  s_pieces[ XYp(s_switch.second) ] = temp;
}


// See if the given location has a match
bool checkLocation(GPoint location) {
  for (int check=0; check < 6; ++check) {
    int xA = 0, xB = 0, yA = 0, yB = 0;
    if        (check == 0 && location.x >= 2) { // X -ve
      xA = -2; xB = -1;
    } else if (check == 1 && location.x >= 1 && location.x <= BOARD_PIECES_X-2) { // X mid
      xA = -1; xB = 1;
    } else if (check == 2  && location.x <= BOARD_PIECES_X-3) { // X +ve
      xA = 1; xB = 2;
    } else if (check == 3 && location.y >= 2) { // Y -ve
      yA = -2; yB = -1;
    } else if (check == 4 && location.y >= 1 && location.y <= BOARD_PIECES_Y-2) { // Y mid
      yA = -1; yB = 1;
    } else if (check == 5 && location.y <= BOARD_PIECES_Y-3) { // Y +ve
      yA = 1; yB = 2;
    } else {
      continue;
    }

    //APP_LOG(APP_LOG_LEVEL_DEBUG, "Check #%i", check);
    bool isValid = checkTriplet( s_pieces[XY(location.x+xA, location.y+yA)].colour,
      s_pieces[XY(location.x+xB, location.y+yB)].colour,
      s_pieces[XYp(location)].colour);
    if (isValid) return true;
  }
  return false;
}

// Need to look two away on either side 0000CCSCC0000. 0=ignore, C=check, S=swap in both V and H
bool checkMove() {

  // we switch the colours temporarily
  switchColours(s_switch.first, s_switch.second);
  bool isValid = false;

  for (int p=0; p<2; ++p) {
    // Look in location A with location B's colour and visa versa
    //APP_LOG(APP_LOG_LEVEL_DEBUG, "Check first/second=%i", p);
    if (p == 0) isValid = checkLocation(s_switch.first);
    if (p == 1) isValid = checkLocation(s_switch.second);
    if (isValid) break;
  }

  // switch back! we will animate the actual pieces into their new positions
  switchColours(s_switch.first, s_switch.second);

  // This routine also finds the computer a valid next move
  if (s_gameState == kFindNextMove) return isValid;

  // If it was user initiated we take action with the FSM
  if (isValid == true) {
    s_currentRun = 0;
    s_gameState = kSwapAnimate;
    s_score.hintOn = 0;
    if (s_hintTimer) app_timer_cancel(s_hintTimer);
    s_hintTimer = NULL;
  } else {
    s_gameState = kNudgeAnimate;
  }
  return false; // This return to the FSM says no need to re-draw
}

void updateSwitchPiecePhysics(int v) {
  if (s_switch.first.x > s_switch.second.x){
    s_pieces[XYp(s_switch.first)].loc.x  -= v;
    s_pieces[XYp(s_switch.second)].loc.x += v;
  } else if (s_switch.second.x > s_switch.first.x) {
    s_pieces[XYp(s_switch.first)].loc.x  += v;
    s_pieces[XYp(s_switch.second)].loc.x -= v;
  } else if (s_switch.first.y > s_switch.second.y){
    s_pieces[XYp(s_switch.first)].loc.y  -= v;
    s_pieces[XYp(s_switch.second)].loc.y += v;
  } else{
    s_pieces[XYp(s_switch.first)].loc.y  += v;
    s_pieces[XYp(s_switch.second)].loc.y -= v;
  }
}


#define NUDGE_MAX_SPEED 70
bool nudgeAnimate() {
  static int v = 0, mode = 0;

  if (mode == 0 && v >= NUDGE_MAX_SPEED) mode = 1;
  else if (mode == 1 && v <= -NUDGE_MAX_SPEED) mode = 2;
  else if (mode == 2 && v >= 0) mode = 3;

  if (mode == 0 || mode == 2) v += GRAVITY;
  else if (mode == 1) v -= GRAVITY;

  updateSwitchPiecePhysics(v);

  if (mode == 3) {
    mode = 0;
    v = 0;
    s_gameState = kIdle;
  }

  return true; // Redraw
}

bool swapAnimate() {
  static int v = 0, mode = 0, travel = 0;
  static GPoint firstStart, secondStart;

  if (mode == 0) {
    firstStart = s_pieces[ XYp(s_switch.first) ].loc;
    secondStart = s_pieces[ XYp(s_switch.second) ].loc;
    mode = 1;
    travel = 0;
    v = 80; // TWEAK
  } else if (mode == 1) {
    v += GRAVITY*2;
    travel += v;
    updateSwitchPiecePhysics(v);
    if (travel / SUB_PIXEL >= PIECE_PIXELS/2)  {
      mode = 2;
      travel = 0;
    }
  } else if (mode == 2) {
    travel += v;
    v -= GRAVITY*2;
    updateSwitchPiecePhysics(v);
    if (travel / SUB_PIXEL >= PIECE_PIXELS/2)  {
      mode = 0;
      s_gameState = kFindMatches;
      // The animation leaves the pices in around the right location, but not exact! Snap them back to the grid
      s_pieces[ XYp(s_switch.first) ].loc = secondStart; // Make switch sub-pixel perfect
      s_pieces[ XYp(s_switch.second) ].loc = firstStart;
      switchPieces(); // Do a full copy
    }
  }

  return true; // Redraw
}

// Return true if the coordinates point to a switched piece
bool isSwitch(int x, int y) {
  if (s_switch.first.x  == -1) return false;
  if (s_switch.first.x  == x && s_switch.first.y == y) return true;
  if (s_switch.second.x == x && s_switch.second.y == y) return true;
  return false;
}

// Explosion always takes preference at the moment. May rebalance?
void explode(MatchType_t dir, int x, int y, Colour_t c) {
  int n = 0; // number of exploded
  if (dir == kRow) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "EXPLODE ROW %i", y);
    for (int X = 0; X < BOARD_PIECES_X; ++X) s_pieces[XY(X,y)].match = kExploded;
  } else if (dir == kColumn){
    APP_LOG(APP_LOG_LEVEL_DEBUG, "EXPLODE COLUMN %i", x);
    for (int Y = 0; Y < BOARD_PIECES_Y; ++Y) s_pieces[XY(x,Y)].match = kExploded;
  } else if (dir == kCross){
    APP_LOG(APP_LOG_LEVEL_DEBUG, "EXPLODE CROSS %i %i", x, y);
    for (int Y = 0; Y < BOARD_PIECES_Y; ++Y) s_pieces[XY(x,Y)].match = kExploded;
    for (int X = 0; X < BOARD_PIECES_X; ++X) s_pieces[XY(X,y)].match = kExploded;
  } else if (dir == kBOOM) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "EXPLODE BIG %i %i", x, y);
    for (int X = x-2; X <= x+2; ++X) {
      for (int Y = y-2; Y <= y+2; ++Y) {
        if (X < 0 || X >= BOARD_PIECES_X) continue;
        if (Y < 0 || Y >= BOARD_PIECES_Y) continue;
        if (X == x-2 && Y == y-2) continue;
        if (X == x+2 && Y == y+2) continue;
        if (X == x-2 && Y == y+2) continue;
        if (X == x+2 && Y == y-2) continue;
        s_pieces[XY(X,Y)].match = kExploded;
        ++n;
      }
    }
  } else if (dir == kColourBoom) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "EXPLODE COLOUR %i", c);
    for (int X = 0; X < BOARD_PIECES_X; ++X) {
      for (int Y = 0; Y < BOARD_PIECES_Y; ++Y) {
        if (s_pieces[XY(X,Y)].colour == c) {
          s_pieces[XY(X,Y)].match = kExploded;
          ++n;
        }
      }
    }
  } else if (dir == kMiniBoom) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "EXPLODE SMALL %i %i col:%i", x, y, c);
    for (int X = x-2; X <= x+2; ++X) {
      for (int Y = y-2; Y <= y+2; ++Y) {
        if (X < 0 || X >= BOARD_PIECES_X) continue;
        if (Y < 0 || Y >= BOARD_PIECES_Y) continue;
        if (s_pieces[XY(X,Y)].colour == c) {
          s_pieces[XY(X,Y)].match = kExploded;
          ++n;
        }
      }
    }
  }
  score(dir, n);
}

/**
 * Iterate the game board and markup for found combinations
 **/
bool findMatches() {
  // One look for explosions

  bool matchesFound = false;

  // Double or singe black? Must be a switch
  if (s_switch.first.x != -1) {
    if (s_pieces[XYp(s_switch.first)].colour == kBlack && s_pieces[XYp(s_switch.second)].colour == kBlack) { // double black
      explode(kBOOM, s_switch.second.x, s_switch.second.y, 0);
      matchesFound = true;
    } else if (s_pieces[XYp(s_switch.first)].colour == kWhite && s_pieces[XYp(s_switch.second)].colour == kBlack) { //White black
      explode(kCross, s_switch.second.x, s_switch.second.y, 0);
      matchesFound = true;
    } else if (s_pieces[XYp(s_switch.first)].colour == kBlack && s_pieces[XYp(s_switch.second)].colour == kWhite) { //Black white
      explode(kCross, s_switch.second.x, s_switch.second.y, 0);
      matchesFound = true;
    } else if (s_pieces[XYp(s_switch.first)].colour == kBlack) { // Single black A
      explode(kMiniBoom, s_switch.second.x, s_switch.second.y, s_pieces[XYp(s_switch.second)].colour);
      s_pieces[XYp(s_switch.first)].match = kExploded;
      matchesFound = true;
    } else if (s_pieces[XYp(s_switch.second)].colour == kBlack) { // Single black B
      explode(kMiniBoom, s_switch.first.x, s_switch.first.y, s_pieces[XYp(s_switch.first)].colour);
      s_pieces[XYp(s_switch.second)].match = kExploded;
      matchesFound = true;
    }
  }

  for (int dir = 0; dir < 2; ++dir) {

    int xStop = 0, yStop = 0, max = 0;
    if (dir == 0) {
      yStop = 2;
      max = BOARD_PIECES_Y;
    } else {
      xStop = 2;
      max = BOARD_PIECES_X;
    }

    for (int x=0; x < BOARD_PIECES_X - xStop; ++x) {
      for (int y=0; y < BOARD_PIECES_Y - yStop; ++y) {
        if (s_pieces[XY(x,y)].match == kExploded) continue; // Don't check if s'ploded

        // Find a run
        int runSize = 1, nextX, nextY;
        if (dir == 0) {
          nextX = x;
          nextY = y+1;
        } else {
          nextX = x+1;
          nextY = y;
        }

        Colour_t runColour = s_pieces[XY(x,y)].colour;

        while (1) {
          if ( s_pieces[XY(x,y)].match == kExploded ) break;
          if (runColour == kWhite) runColour = s_pieces[XY(nextX,nextY)].colour; // If first colour(s) were white - need to update

          else if ( s_pieces[XY(nextX,nextY)].colour == runColour) {} // Progress - Same colour
          else if ( s_pieces[XY(nextX,nextY)].colour == kWhite ) {} // Progress - Wildcard
          else break;

          ++runSize;
          if (dir == 0) ++nextY;
          else          ++nextX;
          if (nextX == max || nextY == max) break;
        }

        if (runSize > 4) {
          // Find colour
          explode(kColourBoom, 0, 0, runColour);
          //s_bombLocation[runColour] = GPoint(x*PIECE_PIXELS + PIECE_PIXELS/2, y*PIECE_PIXELS + PIECE_PIXELS/2);
          //if (dir == 0) s_bombLocation[runColour].y += 2*PIECE_PIXELS; // Centre on the 3rd
          //else s_bombLocation[runColour].x += 2*PIECE_PIXELS;
          matchesFound = true;
        } else if (runSize > 2) {
          APP_LOG(APP_LOG_LEVEL_DEBUG,"MATCH-3");
          runSize == 3 ? score(kMatch3, 0) : score(kMatch4, 0);
          if  (dir == 0) for (int P=y; P < nextY; ++P) s_pieces[XY(x,P)].match++; // A piece can be matched up to twice
          else           for (int P=x; P < nextX; ++P) s_pieces[XY(P,y)].match++;
          matchesFound = true;
        }

        //Promote white
        if (runSize == 4) {
          APP_LOG(APP_LOG_LEVEL_DEBUG,"PROMOTE WHITE");
          if  (dir == 0) {
            for (int P=y; P < nextY; ++P) {
              if (isSwitch(x,P) || P == nextY-1) {
                s_pieces[XY(x,P)].promoteFlag = kWhite;
                break;
              }
            }
          } else {
            for (int P=x; P < nextX; ++P) {
              if (isSwitch(P,y) || P == nextX-1) {
                s_pieces[XY(P,y)].promoteFlag = kWhite;
                break;
              }
            }
          }
        }

        if      (dir == 0 && runSize > 2) y += (runSize-1);
        else if (dir == 1 && runSize > 2) x += (runSize-1);
      }
    }
  }

  // Promote black
  for (int x=0; x < BOARD_PIECES_X; ++x) {
    for (int y=0; y < BOARD_PIECES_Y; ++y) {
      if (s_pieces[XY(x,y)].match == kMatchedTwice) {
        score(kMatchT, 0);
        s_pieces[XY(x,y)].promoteFlag = kBlack;
        APP_LOG(APP_LOG_LEVEL_DEBUG,"PROMOTE BLACK");
      }
    }
  }

  //remember to unset switches so they don't trigger on iterations of this phase
  s_switch.first = GPoint(-1,-1);
  s_switch.second = GPoint(-1,-1);

  if (matchesFound == true) {
    s_gameState = kFlashRemoved;
    ++s_currentRun;
  } else {
     s_gameState = kFindNextMove; // Let the comp find a valid move, if any
   }

  APP_LOG(APP_LOG_LEVEL_DEBUG,"end findMatchs");
  return false; // don't redraw
}

void enableHint(void* data) {
  s_score.hintOn = 1;
  redraw();
  s_hintTimer = NULL;
}

// Find a next legal move
#define HINT_TIMER MS_IN_SEC*20
bool findNextMove() {
  for (int x=0; x < BOARD_PIECES_X; ++x) {
    for (int y=0; y < BOARD_PIECES_Y-1; ++y) {
      s_switch.first = GPoint(x,y);
      s_switch.second = GPoint(x,y+1);
      if (checkMove() == true) { // If valid
        s_availableMove = s_switch.first;
        s_gameState = kIdle;
        if (s_autoMode == true) s_gameState = kCheckMove;
        s_hintTimer = app_timer_register(HINT_TIMER, enableHint, NULL);
        return false; // don't redraw
      }
    }
  }
  for (int x=0; x < BOARD_PIECES_X-1; ++x) {
    for (int y=0; y < BOARD_PIECES_Y; ++y) {
      s_switch.first = GPoint(x,y);
      s_switch.second = GPoint(x+1,y);
      if (checkMove() == true) {
        s_availableMove = s_switch.first;
        s_gameState = kIdle;
        if (s_autoMode == true) s_gameState = kCheckMove;
        s_hintTimer = app_timer_register(HINT_TIMER, enableHint, NULL);
        return false;  // don't redraw
      }
    }
  }

  s_availableMove = GPoint(-1,-1);
  s_gameState = kGameOver;
  return false; // don't redraw
}

bool flashRemoved() {
  static int count = 0;
  if (++count > (ANIM_FPS/2 - s_currentRun)) {
    count = 0;
    //for (int i = 0; i < N_COLOURS; ++i) s_bombLocation[i].x = -1;
    s_gameState = kRemoveAndReplace;
  }
  return true; // redraw
}

bool settleBoard() {
  bool settled = true;
  for (int x=0; x < BOARD_PIECES_X; ++x) {
    for (int y=0; y < BOARD_PIECES_Y; ++y) {
      int floor = y * PIECE_PIXELS * SUB_PIXEL;
      if (s_pieces[XY(x,y)].loc.y < floor) {
        if (s_pieces[XY(x,y)].v == 0) s_pieces[XY(x,y)].v = 80; // TWEEk and make get larger as run progresses?
        s_pieces[XY(x,y)].v += GRAVITY;
        s_pieces[XY(x,y)].loc.y += s_pieces[XY(x,y)].v;
        settled = false;
      } else {
        s_pieces[XY(x,y)].v = 0;
        s_pieces[XY(x,y)].loc.y = floor;
      }
    }
  }

  if (settled) s_gameState = kFindMatches;
  return true; // redraw
}


/**
 * Remove matched pieces, fill in.
 **/
bool removeAndReplace() {
  // REMOVE
  for (int x=0; x < BOARD_PIECES_X; ++x) {
    for (int y=BOARD_PIECES_Y-1; y >= 0; --y) {
      if      (s_pieces[XY(x,y)].promoteFlag != kNONE)      s_pieces[XY(x,y)].colour = s_pieces[XY(x,y)].promoteFlag;
      else if (s_pieces[XY(x,y)].match       != kUnmatched) s_pieces[XY(x,y)].colour = kNONE;
      s_pieces[XY(x,y)].promoteFlag = kNONE;
      s_pieces[XY(x,y)].match = kUnmatched;
    }
  }
  // REPLACE
  for (int x=0; x < BOARD_PIECES_X; ++x) {
    int newPieces = 0;
    for (int y = BOARD_PIECES_Y - 1; y >= 0; --y) {
      if (s_pieces[XY(x,y)].colour != kNONE) continue;
      bool pieceMoved = false;
      for (int Y = y-1; Y >= 0; --Y) {
        if (s_pieces[XY(x,Y)].colour == kNONE) continue;
        s_pieces[XY(x,y)] = s_pieces[XY(x,Y)];
        s_pieces[XY(x,Y)].colour = kNONE; // Set to cascade
        pieceMoved = true;
        break;
      }

      if (pieceMoved == false) {
        // Generate a new piece off the top
        s_pieces[XY(x,y)].loc.y =  -(++newPieces * PIECE_SUB_PIXELS);
        s_pieces[XY(x,y)].colour = (rand() % s_score.nColoursActive) + 1;
      }
    }
  }
  APP_LOG(APP_LOG_LEVEL_DEBUG,"END MOVE");
  s_gameState = kSettleBoard;
  return false; // don't redraw
}

bool awaitingDirection() {
  if (s_frame % 20 == 0) return true; // Flashing cursor only
  return false;
}

bool checkScoreBuffer() {
  if (s_score.pointBuffer == 0) return false;

  // Do we have a little or a lot of points to award?
  if (s_score.pointBuffer >= s_score.pointsToNextLevel/10) s_nWaves = 3; // > 10%
  else if (s_score.pointBuffer >= s_score.pointsToNextLevel/20) s_nWaves = 2; // > 5%
  else if (s_score.pointBuffer >= s_score.pointsToNextLevel/50) s_nWaves = 1; // > 2 %
  else return false; // Not enough

  s_wave[0].origin.y = (s_windowSizeY) * SUB_PIXEL; //size 10
  s_wave[1].origin.y = (s_windowSizeY + 20) * SUB_PIXEL; // gap 10, size 15
  s_wave[2].origin.y = (s_windowSizeY + 45) * SUB_PIXEL; // gap 10, size 20
  s_waveV = 0;

  APP_LOG(APP_LOG_LEVEL_INFO, "buffer has: %i, tot points: %i, pointsToNextLevel: %i", s_score.pointBuffer, s_score.points, s_score.pointsToNextLevel );


  s_score.points += s_score.pointBuffer;
  s_score.pointBuffer = 0;
  if (s_score.points > s_score.pointsToNextLevel) s_score.points = s_score.pointsToNextLevel;

  // Where should the liquid now end?
  int fraction = 100 - ((s_score.points * SUB_PIXEL) / s_score.pointsToNextLevel); // 0 - 100
  s_liquidEnd = (fraction * s_windowSizeY);// [don't divide through, keep in subpixel]  / SUB_PIXEL;

  APP_LOG(APP_LOG_LEVEL_INFO, "fraction: %i, nWaves: %i, Y: %i, Ysp: %i", fraction, s_nWaves, s_liquidEnd/SUB_PIXEL, s_liquidEnd);

  s_scoreState = kApplyPoints;
  return false;
}

bool applyPoints() {
  s_waveV += GRAVITY;
  for (int i = 0; i < 3; ++i) s_wave[i].origin.y -= s_waveV;

  if (s_liquid.origin.y > s_liquidEnd && s_wave[0].origin.y + (5*SUB_PIXEL) < s_liquidEnd) s_liquid.origin.y -= s_waveV;

  if (s_wave[2].origin.y + (s_wave[2].size.h * SUB_PIXEL) < 0) s_scoreState = kCheckNewLevel;
  return true;
}

void updateLevelColour() {
  int nextFG = s_score.level;
  int nextBG = s_score.level - 1;
  while (nextFG >= N_LEVEL_COLOURS) nextFG -= N_LEVEL_COLOURS;
  while (nextBG >= N_LEVEL_COLOURS) nextBG -= N_LEVEL_COLOURS;
  s_colourForground = s_levelColour[ nextFG ];
  s_colourBackground  = s_levelColour[ nextBG ];
}

bool checkNewLevel() {
  if (s_score.points >= s_score.pointsToNextLevel) {
    s_score.points -= s_score.pointsToNextLevel;
    s_score.pointsToNextLevel = (23 * s_score.pointsToNextLevel) / 20; //TODO balance this 23/20 = +15%
    switch (++s_score.level) {
        case 3: s_score.nColoursActive = 6; break;
        case 6: s_score.nColoursActive = 7; break;
        default: break;
    }
    updateLevelColour();
    s_liquidEnd = s_windowSizeY * SUB_PIXEL;
    s_liquid.origin.y = s_windowSizeY * SUB_PIXEL;
  }
  s_nWaves = 0;
  s_scoreState = kWait;
  return true;
}

// TODO
bool gameOver() {
  return false;
}

void gameLoop(void* data) {
  if (++s_frame == ANIM_FPS) s_frame = 0;

  bool requestRedraw = false;

  switch (s_gameState) {
    case kIdle: break;
    case kAwaitingDirection: requestRedraw = awaitingDirection(); break;
    case kCheckMove: requestRedraw = checkMove(); break;
    case kNudgeAnimate: requestRedraw = nudgeAnimate(); break;
    case kSwapAnimate: requestRedraw = swapAnimate(); break;
    case kFindMatches: requestRedraw = findMatches(); break;
    case kFlashRemoved: requestRedraw = flashRemoved(); break;
    case kRemoveAndReplace: requestRedraw = removeAndReplace(); break;
    case kSettleBoard: requestRedraw = settleBoard(); break;
    case kFindNextMove: requestRedraw = findNextMove(); break;
    case kGameOver: requestRedraw = gameOver(); break;
    default: break;
  }

  switch (s_scoreState) {
    case kWait: requestRedraw |= checkScoreBuffer(); break;
    case kApplyPoints: requestRedraw |= applyPoints(); break;
    case kCheckNewLevel: requestRedraw |= checkNewLevel(); break;
    default: break;
  }

  // only if taking acceleromiter data do we ALWAY redraw
  if (getTiltStatus() > 0 || requestRedraw == true) redraw();

  if (s_frame % 100 == 0)  APP_LOG(APP_LOG_LEVEL_DEBUG, "game looping still");
  s_gameLoopTime = app_timer_register(ANIM_DELAY, gameLoop, NULL);
}

/**  Called when a direction key is pressed when in SelectDirection mode
 *   OR whenever a new cell is entered by the accelerometer
 **/
void checkSwitch(int x, int y) {
  if (s_gameState != kAwaitingDirection) return;

  // Check out of bounds (for button instagated)
  if (y < 0 || y >= BOARD_PIECES_Y || x < 0 || x >= BOARD_PIECES_X) {
    redraw(); //  why?
    return;
  }

  s_switch.second.x = x;
  s_switch.second.y = y;

  s_gameState = kCheckMove;
}

static void dataHandler(AccelData* data, uint32_t num_samples) {
  // Update
  s_motionCursor.x += data[0].x / getTiltStatus(); // 0=off, 1=high, 2=low
  s_motionCursor.y -= data[0].y / getTiltStatus();

  if (s_motionCursor.x < 0) s_motionCursor.x += BOARD_SIZE_X * SUB_PIXEL;
  else if (s_motionCursor.x > BOARD_SIZE_X * SUB_PIXEL) s_motionCursor.x -= BOARD_SIZE_X * SUB_PIXEL;

  if (s_motionCursor.y < 0) s_motionCursor.y += BOARD_SIZE_Y * SUB_PIXEL;
  else if (s_motionCursor.y > BOARD_SIZE_Y * SUB_PIXEL) s_motionCursor.y -= BOARD_SIZE_Y * SUB_PIXEL;

  GPoint before = s_cursor;

  // Translate
  s_cursor.x = s_motionCursor.x / (PIECE_SUB_PIXELS); //Note: quotes due to macro
  s_cursor.y = s_motionCursor.y / (PIECE_SUB_PIXELS);

  if (before.x != s_cursor.x || before.y != s_cursor.y) {
    checkSwitch(s_cursor.x, s_cursor.y); // Check the square i just moved into
  }

}

void tiltMode() {
  if (getTiltStatus() > 0) {
    accel_data_service_subscribe(1, dataHandler);
    accel_service_set_sampling_rate(ACCEL_SAMPLING_25HZ);
  } else{
    accel_data_service_unsubscribe();
  }
}

void newGame(bool doLoadGame) {
  // Zero data store
  // Keep the best level data
  APP_LOG(APP_LOG_LEVEL_WARNING,"B");
  memset(&s_pieces, 0, BOARD_PIECES_X * BOARD_PIECES_Y * sizeof(Piece_t));
  memset(&s_score, 0, sizeof(Score_t));
  if (doLoadGame == true) {
    loadGame();
    updateLevelColour();
  } else { // new game
    // Init score
    s_score.level = 1;
    s_score.bestLevel = getBestLevel();
    s_score.lives = 3;
    s_score.pointsToNextLevel = 200;
    s_score.nColoursActive = 5;
    s_colourForground = s_levelColour[ 1 ];
    s_colourBackground  = s_levelColour[ 0 ];
  }
  APP_LOG(APP_LOG_LEVEL_WARNING,"C");
  int offset = BOARD_SIZE_Y * SUB_PIXEL;
  for (int y = BOARD_PIECES_Y-1; y >= 0; --y) {
    for (int x = 0; x < BOARD_PIECES_X; ++x) {
      if (doLoadGame == false) {
        bool placing = true;
        while (placing) {
          s_pieces[ XY(x,y) ].colour = (rand() % s_score.nColoursActive) + 1;
          placing = checkLocation(GPoint(x,y)); // if match found then we're still placing as we need to try again
        }
      }
      s_pieces[ XY(x,y) ].loc.x = x * PIECE_SUB_PIXELS; // Set location
      s_pieces[ XY(x,y) ].loc.y = (y * PIECE_SUB_PIXELS) - offset;// - (rand() % PIECE_SUB_PIXELS);
    }
    offset += PIECE_SUB_PIXELS;// + (rand() % PIECE_SUB_PIXELS);
  }
  APP_LOG(APP_LOG_LEVEL_WARNING,"D");
  tiltMode();
  s_scoreState = kWait;
  s_gameState = kSettleBoard;
  APP_LOG(APP_LOG_LEVEL_WARNING,"E");
}

void mainWindowClickHandler(ClickRecognizerRef recognizer, void *context) {
  ButtonId button = click_recognizer_get_button_id(recognizer);
  APP_LOG(APP_LOG_LEVEL_DEBUG,"got %i", button);

  if (s_gameState == kAwaitingDirection) {

    // Check the direction I just pressed
    if      (BUTTON_ID_UP     == button) checkSwitch(s_cursor.x    , s_cursor.y - 1);
    else if (BUTTON_ID_SELECT == button) checkSwitch(s_cursor.x + 1, s_cursor.y    );
    else if (BUTTON_ID_DOWN   == button) checkSwitch(s_cursor.x    , s_cursor.y + 1);
    else if (BUTTON_ID_BACK   == button) checkSwitch(s_cursor.x - 1, s_cursor.y    );

  } else {

    if      (BUTTON_ID_UP == button && getTiltStatus() == 0) --s_cursor.y;
    else if (BUTTON_ID_UP == button && getTiltStatus() >  0) s_motionCursor.y -= PIECE_SUB_PIXELS;
    else if (BUTTON_ID_SELECT == button && getTiltStatus() == 0) ++s_cursor.x;
    else if (BUTTON_ID_SELECT == button && getTiltStatus() >  0) s_motionCursor.x += PIECE_SUB_PIXELS;
    else if (BUTTON_ID_DOWN == button && s_gameState == kIdle) {
      s_gameState = kAwaitingDirection;
      s_switch.first = s_cursor;
    } else if (BUTTON_ID_BACK == button) {

      pushSplashWindow();
    }

    if (s_cursor.x >= BOARD_PIECES_X) s_cursor.x = 0;
    if (s_cursor.y < 0) s_cursor.y = BOARD_PIECES_Y - 1;

    redraw();
  }
}

void mainWindowClickConfigProvider(Window *window) {
  window_single_repeating_click_subscribe(BUTTON_ID_SELECT, 100, mainWindowClickHandler);
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, mainWindowClickHandler);
  window_single_click_subscribe(BUTTON_ID_DOWN, mainWindowClickHandler);
  window_single_click_subscribe(BUTTON_ID_BACK, mainWindowClickHandler);
}

static void mainWindowUpdateProc(Layer* this_layer, GContext *ctx) {

    graphics_context_set_fill_color(ctx, s_colourBackground);
    graphics_fill_rect(ctx, layer_get_bounds(this_layer), 0, GCornersAll);
    graphics_context_set_fill_color(ctx, s_colourForground);
    GRect L = s_liquid;
    L.origin.y /= SUB_PIXEL;
    graphics_fill_rect(ctx, L, 0, GCornersAll);
    graphics_context_set_fill_color(ctx, s_colourBackground);
    if (s_nWaves) {
      for (int i = 0; i < s_nWaves; ++i) {
        GRect W = s_wave[i];
        W.origin.y /= SUB_PIXEL;
        graphics_fill_rect(ctx, W, 0, GCornersAll);
      }
    }

    GRect b = layer_get_bounds(this_layer);
    GRect levelRect = GRect( ((b.size.w - BOARD_SIZE_X)/2), b.size.h - 15, PIECE_PIXELS, 15);
    static char levelBuffer[5];
    snprintf(levelBuffer, 5, "%i", s_score.level);
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, levelBuffer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD), levelRect, GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

    GPoint lives = GPoint( b.size.w/2 - PIECE_PIXELS, b.size.h - 6);
    for (int L=0; L<3; ++L) {
      GColor in;
      GColor out;
      if (s_score.lives - L > 0) {
        if (L == 0) { in = GColorMintGreen; out = GColorDarkGreen; }
        else if (L == 1) { in = GColorRajah; out = GColorWindsorTan; }
        else { in = GColorMelon; out = GColorDarkCandyAppleRed; }
      } else {
        in = GColorLightGray;
        out = GColorDarkGray;
      }
      GPoint p = lives;
      p.x += PIECE_PIXELS * L;
      graphics_context_set_stroke_color(ctx, out);
      graphics_context_set_fill_color(ctx, in);
      graphics_context_set_stroke_width(ctx, 3);
      graphics_fill_circle(ctx, p, 3);
      graphics_draw_circle(ctx, p, 3);
    }

}

static void boardUpdateProc(Layer* this_layer, GContext *ctx) {
  graphics_context_set_antialiased(ctx, 0);

  // Fill back
  graphics_context_set_fill_color(ctx, GColorLightGray);
  graphics_fill_rect(ctx, GRect(0, 0, BOARD_SIZE_X, BOARD_SIZE_Y), 0, GCornersAll);

  // Fill highight
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, GRect(s_cursor.x * PIECE_PIXELS, 0, PIECE_PIXELS, BOARD_SIZE_Y), 0, GCornersAll);
  graphics_fill_rect(ctx, GRect(0, s_cursor.y * PIECE_PIXELS, BOARD_SIZE_X, PIECE_PIXELS), 0, GCornersAll);

  graphics_context_set_stroke_color(ctx, GColorBlack);
  // Draw frame
  for (int x = 0; x < BOARD_PIECES_X; ++x) {
    if (x%2 == 1 && x != BOARD_PIECES_X - 1) continue;
    graphics_draw_rect(ctx, GRect(x*PIECE_PIXELS, 0, PIECE_PIXELS+1, BOARD_SIZE_Y+1));
  }
  for (int y = 0; y < BOARD_PIECES_Y; ++y) {
    if (y%2 == 1 && y != BOARD_PIECES_Y - 1) continue;
    graphics_draw_rect(ctx, GRect(0, y*PIECE_PIXELS, BOARD_SIZE_X+1, PIECE_PIXELS+1));
  }

  // Frame highlight
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_draw_rect(ctx, GRect(s_cursor.x*PIECE_PIXELS, s_cursor.y*PIECE_PIXELS, PIECE_PIXELS+1, PIECE_PIXELS+1));

  // Fill shapes
  for (int x = 0; x < BOARD_PIECES_X; ++x) {
    for (int y = 0; y < BOARD_PIECES_Y; ++y) {
      graphics_context_set_stroke_color(ctx, GColorBlack);
      int xy = XY(x,y);
      if (s_gameState == kFlashRemoved && s_pieces[xy].match != kUnmatched) {
        GColor highlight;
        if      (s_pieces[xy].match == kMatchedOnce)  highlight = GColorRajah;
        else if (s_pieces[xy].match == kMatchedTwice) highlight = GColorWindsorTan;
        else if (s_pieces[xy].match == kExploded)     {
          highlight = GColorRoseVale;
          //if (s_bombLocation[ s_pieces[xy].colour ].x != -1) { // Draw explosion line
          //  GPoint myCentre = GPoint( s_pieces[xy].loc.x / SUB_PIXEL, s_pieces[xy].loc.y / SUB_PIXEL ) ;
          //  myCentre.x += PIECE_PIXELS/2;
          //  myCentre.y += PIECE_PIXELS/2;
          //  graphics_context_set_stroke_color(ctx, GColorRed);
          //  graphics_context_set_stroke_width(ctx, 3);
          //  graphics_draw_line(ctx, myCentre, s_bombLocation[ s_pieces[xy].colour ]);
          //  graphics_context_set_stroke_width(ctx, 1);
          //}
        }
        graphics_context_set_fill_color(ctx, highlight);
        graphics_fill_rect(ctx, GRect((s_pieces[xy].loc.x/SUB_PIXEL)+1, (s_pieces[xy].loc.y/SUB_PIXEL)+1, PIECE_PIXELS-1, PIECE_PIXELS-1), 0, GCornersAll);
      }
      switch (s_pieces[xy].colour) {
        case kRed: graphics_context_set_fill_color(ctx, GColorRed); break;
        case kYellow: graphics_context_set_fill_color(ctx, GColorYellow); break;
        case kBlue: graphics_context_set_fill_color(ctx, GColorElectricUltramarine); break;
        case kGreen: graphics_context_set_fill_color(ctx, GColorGreen); break;
        case kPurple: graphics_context_set_fill_color(ctx, GColorCeleste); break;
        case kOrange: graphics_context_set_fill_color(ctx, GColorOrange); break;
        case kPink: graphics_context_set_fill_color(ctx, GColorRichBrilliantLavender); break;
        case kWhite: graphics_context_set_fill_color(ctx, GColorWhite); break;
        case kBlack: graphics_context_set_fill_color(ctx, GColorBlack); break;
        case kNONE: APP_LOG(APP_LOG_LEVEL_ERROR,"Try to draw kNONE at %i %i",x,y); break;
        default: continue;
      }
      if (getShape( s_pieces[xy].colour ) != NULL) {
        gpath_move_to(getShape( s_pieces[xy].colour ), GPoint(s_pieces[xy].loc.x/SUB_PIXEL, s_pieces[xy].loc.y/SUB_PIXEL));
        gpath_draw_filled(ctx, getShape( s_pieces[xy].colour ));
        gpath_draw_outline(ctx, getShape( s_pieces[xy].colour ));
      } else {
        graphics_fill_rect(ctx, GRect((s_pieces[xy].loc.x/SUB_PIXEL)+2, (s_pieces[xy].loc.y/SUB_PIXEL)+2, PIECE_PIXELS-3, PIECE_PIXELS-3), 2, GCornersAll);
      }
    }
  }

  // Move Arrows
  if (s_frame < 20 && s_gameState == kAwaitingDirection) {
    graphics_context_set_fill_color(ctx, GColorWhite);
    for (int d = 0; d < N_CARDINAL; ++d) {
      gpath_move_to(getArrow(d), GPoint(s_cursor.x * PIECE_PIXELS, s_cursor.y * PIECE_PIXELS));
      gpath_draw_filled(ctx, getArrow(d));
      gpath_draw_outline(ctx, getArrow(d));
    }
  }

  // Cursor
  if (getTiltStatus() > 0) {
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_fill_circle(ctx, GPoint(s_motionCursor.x/SUB_PIXEL,s_motionCursor.y/SUB_PIXEL), 3);
    graphics_draw_circle(ctx, GPoint(s_motionCursor.x/SUB_PIXEL,s_motionCursor.y/SUB_PIXEL), 3);
  }

  // Next move
  if (s_score.hintOn && s_availableMove.x != -1 && s_gameState == kIdle) {
    graphics_context_set_stroke_color(ctx, GColorDarkCandyAppleRed);
    graphics_context_set_stroke_width(ctx, 3);
    graphics_draw_circle(ctx, GPoint(s_availableMove.x*PIECE_PIXELS + PIECE_PIXELS/2, s_availableMove.y*PIECE_PIXELS + PIECE_PIXELS/2), PIECE_PIXELS);
    graphics_context_set_stroke_width(ctx, 1);
  }

  // Redo border
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_rect(ctx, GRect(0, 0, BOARD_SIZE_X+1, BOARD_SIZE_Y+1));

}

void mainWindowLoad(Window* parentWindow) {
  GRect b = layer_get_bounds( window_get_root_layer(parentWindow) );
  s_windowSizeY = b.size.h - STATUS_BAR_LAYER_HEIGHT;

  s_statusBar = status_bar_layer_create();
  layer_add_child(window_get_root_layer(parentWindow), status_bar_layer_get_layer(s_statusBar));
  status_bar_layer_set_separator_mode(s_statusBar, StatusBarLayerSeparatorModeDotted);

  s_mainWindowLayer = layer_create( GRect(0, STATUS_BAR_LAYER_HEIGHT, b.size.w, b.size.h - STATUS_BAR_LAYER_HEIGHT) );
  layer_add_child(window_get_root_layer(parentWindow), s_mainWindowLayer);
  layer_set_update_proc(s_mainWindowLayer, mainWindowUpdateProc);

  int disp = (b.size.w - BOARD_SIZE_X) / 2;

  s_boardLayer = layer_create( GRect(disp, disp, BOARD_SIZE_X + 1, BOARD_SIZE_Y + 1) );
  layer_add_child(s_mainWindowLayer, s_boardLayer);
  layer_set_update_proc(s_boardLayer, boardUpdateProc);
  layer_set_clips(s_boardLayer, true);


  s_wave[0] = GRect(0, b.size.h * SUB_PIXEL, b.size.w, 10);
  s_wave[1] = GRect(0, b.size.h * SUB_PIXEL, b.size.w, 15);
  s_wave[2] = GRect(0, b.size.h * SUB_PIXEL, b.size.w, 20);
  s_liquid  = GRect(0, b.size.h * SUB_PIXEL, b.size.w, b.size.h); // TODO x2 for safety
  s_nWaves = 0;
  s_liquidEnd = s_windowSizeY * SUB_PIXEL;

  s_levelColour[0] = GColorTiffanyBlue;
  s_levelColour[1] = GColorMediumAquamarine;
  s_levelColour[2] = GColorVividCerulean;
  s_levelColour[3] = GColorSpringBud;
  s_levelColour[4] = GColorBlueMoon;
  s_levelColour[5] = GColorYellow;
  s_levelColour[6] = GColorChromeYellow;
  s_levelColour[7] = GColorSunsetOrange;
  s_levelColour[8] = GColorMelon;
  s_levelColour[9] = GColorPurple;
  s_levelColour[10] = GColorRichBrilliantLavender;
  s_levelColour[11] = GColorLiberty;
  s_levelColour[12] = GColorWhite;

  s_currentRun = 0;
  s_switch.first = GPoint(-1,-1);
  s_switch.second = GPoint(-1,-1);
  s_cursor = GPoint(0,0);
  s_motionCursor = GPoint(PIECE_SUB_PIXELS/2,PIECE_SUB_PIXELS/2);
  s_availableMove = GPoint(-1,-1);

  srand(time(NULL));
  gameLoop(NULL);

}

void mainWindowUnload() {
  if (s_gameState == kGameOver) endGame();
  else saveGame();
  layer_destroy(s_boardLayer);
  layer_destroy(s_mainWindowLayer);
  status_bar_layer_destroy(s_statusBar);
  s_boardLayer = NULL;
  s_mainWindowLayer = NULL;
  s_statusBar = NULL;
  if (s_gameLoopTime) app_timer_cancel(s_gameLoopTime);
  s_gameLoopTime = NULL;
  if (s_hintTimer) app_timer_cancel(s_hintTimer);
  s_hintTimer = NULL;
  accel_data_service_unsubscribe();
}
