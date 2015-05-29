/**
 *  Shinonome -- A console-based BMS player.
 *  Copyright (C) 2015  Kazumi Moriya <kuroclef@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <climits>
#include <curses.h>
#include <fstream>
#include <numeric>
#include <regex>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <thread>
#include <unistd.h>

#define MAX_SPEED          5.00
#define LANES_COUNT        8
#define LIFETIME_BEATS     5
#define JUDGE_BORDER_COOL  0.025
#define JUDGE_BORDER_GREAT 0.050
#define JUDGE_BORDER_GOOD  0.100
#define JUDGE_PHASES       4

using std::string;

struct Option {
  double speed;
  string keyBinds;
  int    autoPlay;
  string bmsFile;
};

struct Chip {
  double beat;
  double beat2;
  double value;
};

struct Segment {
  double time;
  double beat;
  double velocity;
  double bpm;
};

using Beats      = std::array<double, 1000>;
using Table      = std::array<double, 1296>;
using ChunkTable = std::array<Mix_Chunk *, 1296>;
using Chips      = std::vector<Chip>;
using Chips_i    = std::vector<Chip>::iterator;
using Segments   = std::vector<Segment>;
using Segments_i = std::vector<Segment>::iterator;

struct BMS {
  string     title;
  string     artist;
  string     genre;
  string     level;
  string     lnobj;
  Beats      beats;
  Table      bpmTable;
  Table      stopTable;
  ChunkTable chunkTable;
  Chips      bgms;
  Chips      bpms;
  Chips      lanes[LANES_COUNT];
  Segments   segments;
  string     basePath;
  int        totalNotes;
};

struct Lane {
  Chips_i begin;
  Chips_i end;
};

struct Player {
  string     title;
  string     artist;
  string     genre;
  string     level;
  ChunkTable chunkTable;
  Segments_i segments;
  double     startTime;
  double     beat;
  double     bpm;
  Chips_i    bgm;
  Lane       lanes[LANES_COUNT];
  unsigned   inputs[LANES_COUNT];
  int        judges[LANES_COUNT];
  int        totalNotes;
  int        gameover;
  int        quit;
};

struct Score {
  int judges[JUDGE_PHASES];
  int combo;
  int comboBonus;
  int maxCombo;
  int point;
  int totalJudges;
  int totalNotes;
};

struct Point {
  int x;
  int y;
};

using Points = std::vector<Point>;

void     readArgs(int, char **, Option &);

void     parseBMS(Option &, BMS &);
void     parseBMSMeasure(BMS &, string &);
void     parseBMSCommand(BMS &, string &);
void     parseBMSChannel(BMS &, string &);
void     bindChunkTable(ChunkTable &, string &&, string &&, string &);
void     calcChip(Chips &, int, string &&, BMS &, int);
void     calcSegment(Chips &, Segments &);
double   measureToBeat(double *, int);

void     newGame(BMS &, Player &);
void     play(Player &, Option &, Score &);
double   getTime(void);
Segment &getSegment(Segments_i &, double);
void     update(Player &, Option &, Score &);
void     render(Player &, Option &, Score &);
int      getPos(Player &, Option &, double, int);
void     blit(int, int, Points &);
void     drawBar(int, int, int, Points &);
void     judge(Player &, Option &, Score &, Lane &, int);
void     judgeln(Player &, Option &, Score &, Lane &, int);
void     calculate(Score &, int);
void     calcReset(Score &);
void     comboCount(Score &);
void     comboBonus(Score &);
void     scoreCount(Score &);
void     handler(Player &, Option &, Score &);
void     gameOver(Player &, Score &);
void     printScore(Player &, Option &, Score &);

void     printHelp(void);

int main(int argc, char **argv) {
  if (argc == 1) printHelp();

  Option option = {}; readArgs(argc, argv, option);

  SDL_Init(SDL_INIT_AUDIO);
  Mix_OpenAudio(MIX_DEFAULT_FREQUENCY * 2, MIX_DEFAULT_FORMAT, 2, 1024);
  Mix_AllocateChannels(std::tuple_size<ChunkTable>{});

  BMS    bms    = {}; parseBMS(option, bms);
  Player player = {}; newGame(bms, player);
  Score  score  = {}; play(player, option, score);

  printScore(player, option, score);

  Mix_CloseAudio();
  SDL_Quit();

  return EXIT_SUCCESS;
}

void readArgs(int argc, char **argv, Option &option) {
  option.speed    = 1.00;
  option.keyBinds = "azsxdcfv";

  int arg = 0;
  while ((arg = getopt(argc, argv, "s:k:ah")) != -1) {
    double speed = 0;
    switch (arg) {
    case 's':
      speed = std::atof(optarg);
      if (speed < 1.00 || MAX_SPEED < speed) continue;
      option.speed = speed;
      continue;

    case 'k':
      option.keyBinds = optarg;
      continue;

    case 'a':
      option.autoPlay = 1;
      continue;

    default:
      printHelp();
    }
  }
  option.bmsFile = argv[optind];
}

void parseBMS(Option &option, BMS &bms) {
  bms.bgms.push_back({ INT_MAX, 0, 0 });
  bms.bpms.push_back({ INT_MAX, 0, 0 });
  for (int i = 0; i < LANES_COUNT; i++)
    bms.lanes[i].push_back({ INT_MAX, 0, 0 });
  bms.beats.fill(4);

  auto pos = option.bmsFile.find_last_of('/');
  if (pos != string::npos)
    bms.basePath = option.bmsFile.substr(0, pos) + "/";

  std::ifstream stream(option.bmsFile);
  if (stream.fail()) std::exit(EXIT_FAILURE);

  string line;
  while (std::getline(stream, line))
    parseBMSMeasure(bms, line);
  stream.close();

  stream.open(option.bmsFile);
  while (std::getline(stream, line))
    parseBMSCommand(bms, line);
  stream.close();

  calcSegment(bms.bpms, bms.segments);
}

void parseBMSMeasure(BMS &bms, string &line) {
  std::smatch m;
  std::regex  r("#(\\d{3})02:([.0-9]+)\\r?");
  if (!std::regex_match(line, m, r)) return;
  bms.beats[std::stoi(m[1])] = std::stof(m[2]) * 4;
}

void parseBMSCommand(BMS &bms, string &line) {
  std::smatch m;
  std::regex  r("#(\\w+)\\ (.+)\\r?");
  if (!std::regex_match(line, m, r)) {
    parseBMSChannel(bms, line);
    return;
  }

  if (m[1] == "TITLE") {
    bms.title = m[2];
    return;
  }

  if (m[1] == "ARTIST") {
    bms.artist = m[2];
    return;
  }

  if (m[1] == "GENRE") {
    bms.genre = m[2];
    return;
  }

  if (m[1] == "PLAYLEVEL") {
    bms.level = m[2];
    return;
  }

  if (m[1] == "LNOBJ") {
    bms.lnobj = m[2];
    return;
  }

  if (m[1] == "BPM") {
    bms.bpms.insert(bms.bpms.cbegin(), { 0, 0, std::stof(m[2]) });
    return;
  }

  string str(m[1]);
  std::smatch n;
  std::regex  s("(\\w+)(\\w{2})");
  if (!std::regex_match(str, n, s)) return;

  if (n[1] == "BPM") {
    bms.bpmTable[std::stoi(n[2], NULL, 36)] = std::stof(m[2]);
    return;
  }

  if (n[1] == "STOP") {
    bms.stopTable[std::stoi(n[2], NULL, 36)] = std::stof(m[2]) / 48;
    return;
  }

  if (n[1] == "WAV") {
    bindChunkTable(bms.chunkTable, n[2], m[2], bms.basePath);
    return;
  }
}

void parseBMSChannel(BMS &bms, string &line) {
  std::smatch m;
  std::regex  r("#(\\d{3})(\\d{2}):(\\w+)\\r?");
  if (!std::regex_match(line, m, r)) return;

  int c = 0;
  int channel = std::stoi(m[2]);
  switch (channel) {
  case 1:
    calcChip(bms.bgms, std::stoi(m[1]), m[3], bms, channel);
    return;

  case 3: case 8 ... 9:
    calcChip(bms.bpms, std::stoi(m[1]), m[3], bms, channel);
    return;

  case 11 ... 16: case 18 ... 19:
    if (channel <= 15) c = channel - 10;
    if (channel >= 18) c = channel - 12;
    calcChip(bms.lanes[c], std::stoi(m[1]), m[3], bms, channel);
    return;

  case 51 ... 56: case 58 ... 59:
    if (channel <= 55) c = channel - 50;
    if (channel >= 58) c = channel - 52;
    calcChip(bms.lanes[c], std::stoi(m[1]), m[3], bms, channel);
    return;
  }
}

void bindChunkTable(ChunkTable &chunkTable, string &&index, string &&path, string &basePath) {
  auto &chunk = chunkTable[std::stoi(index, NULL, 36)];
  if (chunk != NULL) Mix_FreeChunk(chunk);

  std::replace(path.begin(), path.end(), '\\', '/');
  auto pos = path.find_last_of('.');
  if (pos != string::npos) path.erase(pos, path.size());

  for (auto e : { ".ogg", ".wav" }) {
    string fullPath = basePath + path + e;
    chunk = Mix_LoadWAV(fullPath.c_str());
    if (chunk != NULL) return;
  }
}

void calcChip(Chips &chips, int measure, string &&notation, BMS &bms, int channel) {
  int    length    = notation.size() / 2;
  double init_beat = std::accumulate(bms.beats.cbegin(), bms.beats.cbegin() + measure, 0.0);

  int i = 0;
  for (int c = 0; c < length; c++) {
    string str(&notation[c * 2], 2);
    if (str == "00") continue;

    double beat = init_beat + c * bms.beats[measure] / length;
    while (chips.at(i).beat <= beat) i++;

    double value = 0;
    switch (channel) {
    case 1:
      value = std::stoi(str, NULL, 36);
      chips.insert(chips.cbegin() + i, { beat, 0, value });
      continue;

    case 3:
      value = std::stoi(str, NULL, 16);
      chips.insert(chips.cbegin() + i, { beat, 0, value });
      continue;

    case 8:
      value = bms.bpmTable[std::stoi(str, NULL, 36)];
      chips.insert(chips.cbegin() + i, { beat, 0, value });
      continue;

    case 9:
      value = bms.stopTable[std::stoi(str, NULL, 36)];
      chips.insert(chips.cbegin() + i, { beat, value, 0 });
      continue;

    case 11 ... 16: case 18 ... 19:
      if (i && str == bms.lnobj) {
        chips.at(i - 1).beat2 = beat;
        continue;
      }

      value = std::stoi(str, NULL, 36);
      chips.insert(chips.cbegin() + i, { beat, 0, value });
      bms.totalNotes++;
      continue;

    case 51 ... 56: case 58 ... 59:
      if (i && chips.at(i - 1).beat2 < 0) {
        chips.at(i - 1).beat2 = beat;
        continue;
      }

      value = std::stoi(str, NULL, 36);
      chips.insert(chips.cbegin() + i, { beat, -1, value });
      bms.totalNotes++;
      continue;
    }
  }
}

void calcSegment(Chips &bpms, Segments &segments) {
  double state_time = 0;
  double state_beat = 0;
  double state_bpm  = bpms.front().value;
  segments.push_back({ 0, 0, state_bpm / 60, state_bpm });

  for (auto &bpm : bpms) {
    double beat = bpm.beat;
    double time = state_time + (beat - state_beat) * 60 / state_bpm;
    if (bpm.value > 0) {
      state_bpm  = bpm.value;
      segments.push_back({ time, beat, state_bpm / 60, state_bpm });
      state_time = time;
      state_beat = beat;
      continue;
    }

    if (bpm.beat2 > 0) {
      segments.push_back({ time, beat, 0, state_bpm });
      time += bpm.beat2 * 60 / state_bpm;
      segments.push_back({ time, beat, state_bpm / 60, state_bpm });
      state_time = time;
      state_beat = beat;
      continue;
    }
  }
  segments.push_back({ INT_MAX, INT_MAX, 0, 0 });
}

double measureToBeat(double *beats, int measure) {
  double beat = 0;
  for (int i = 0; i < measure; i++) beat += beats[i];
  return beat;
}

void newGame(BMS &bms, Player &player) {
  player.title      = std::move(bms.title);
  player.artist     = std::move(bms.artist);
  player.genre      = std::move(bms.genre);
  player.level      = std::move(bms.level);
  player.chunkTable = std::move(bms.chunkTable);
  player.segments   = bms.segments.begin();
  player.bgm        = bms.bgms.begin();
  player.totalNotes = bms.totalNotes;

  for (int i = 0; i < LANES_COUNT; i++)
    player.lanes[i] = { bms.lanes[i].begin(), bms.lanes[i].begin() };
}

void play(Player &player, Option &option, Score &score) {
  score.totalNotes = player.totalNotes;

  std::setlocale(LC_ALL, "");
  initscr();
  cbreak();
  noecho();
  curs_set(0);
  timeout(0);

  start_color();
  use_default_colors();
  init_pair(COLOR_RED , COLOR_RED , -1);
  init_pair(COLOR_BLUE, COLOR_BLUE, -1);
  std::freopen("/dev/null", "w", stderr);

  player.startTime = getTime();

  while (!player.quit) {
    update(player, option, score);
    render(player, option, score);
    std::this_thread::yield();
  }

  endwin();
}

double getTime() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

Segment &getSegment(Segments_i &segments, double time) {
  while ((segments + 1)->time <= time) segments++;
  return *segments;
}

void update(Player &player, Option &option, Score &score) {
  double   currentTime = (getTime() - player.startTime) / 1000;
  Segment &segment     = getSegment(player.segments, currentTime);

  player.beat = segment.beat + (currentTime - segment.time) * segment.velocity;
  player.bpm  = segment.bpm;

  while (player.beat >= player.bgm->beat) {
    int ch = player.bgm->value;
    Mix_PlayChannel(ch, player.chunkTable.at(ch), 0);
    player.bgm++;
  }

  double padding = 0;
  if (!option.autoPlay) padding = (JUDGE_BORDER_GOOD + 0.001) * player.bpm / 60;

  for (int i = 0; i < LANES_COUNT; i++) {
    Lane &lane = player.lanes[i];
    if (player.beat >= lane.begin->beat + padding)
      judge(player, option, score, lane, i);

    if (player.beat >= lane.end->beat - LIFETIME_BEATS)
      lane.end++;

    if (player.judges[i])
      judgeln(player, option, score, lane, i);
  }

  if (player.bgm->beat >= INT_MAX)
    if (!Mix_Playing(-1)) gameOver(player, score);

  static double lastHandledTime;
  if ((currentTime - lastHandledTime) * 1000 >= 0xf) {
    handler(player, option, score);
    lastHandledTime = currentTime;
  }
}

void render(Player &player, Option &option, Score &score) {
  int w = getmaxx(stdscr);
  int h = getmaxy(stdscr);

  static Points buffer;
  for (auto &point : buffer)
    mvaddstr(point.y, point.x, "        ");
  buffer.clear();

  for (int i = 0; i < LANES_COUNT; i++) {
    switch (i) {
    case 0:
      attrset(A_BOLD | COLOR_PAIR(COLOR_RED));
      break;

    case 1: case 3: case 5: case 7:
      attrset(A_BOLD | COLOR_PAIR(COLOR_BLACK));
      break;

    case 2: case 4: case 6:
      attrset(A_BOLD | COLOR_PAIR(COLOR_BLUE));
      break;
    }

    Lane    &lane = player.lanes[i];
    Chips_i  note = lane.begin;
    while (note->beat < lane.end->beat) {
      int y = getPos(player, option, note->beat, h);
      if (y <  0) break;
      if (y >= h) y = h - 1;

      if (note->beat2 <= 0) {
        blit(y, i, buffer);
        note++;
        continue;
      }

      int y2 = getPos(player, option, note->beat2, h);

      drawBar(y, y2, i, buffer);
      blit(y , i, buffer);
      blit(y2, i, buffer);
      note++;
    }
  }
  attrset(0);

  mvprintw(0, w - player.genre.size() , "%s", player.genre.c_str());
  mvprintw(1, w - player.title.size() , "%s", player.title.c_str());
  mvprintw(2, w - player.artist.size(), "%s", player.artist.c_str());

  mvprintw(4, w - player.level.size() , "%s", player.level.c_str());
  mvprintw(5, w - 14, "%14.2f", player.bpm);
  mvprintw(6, w - 14, "%14.2f", option.speed);
  mvaddstr(4, w - 14, "Level : ");
  mvaddstr(5, w - 14, "BPM   : ");
  mvaddstr(6, w - 14, "Speed : ");

  mvprintw(h - 6, 8 * LANES_COUNT, "%6d", score.judges[0]);
  mvprintw(h - 5, 8 * LANES_COUNT, "%6d", score.judges[1]);
  mvprintw(h - 4, 8 * LANES_COUNT, "%6d", score.judges[2]);
  mvprintw(h - 3, 8 * LANES_COUNT, "%6d", score.judges[3]);

  mvprintw(h - 1, 8 * LANES_COUNT, "%6d", score.combo);

  refresh();
}

int getPos(Player &player, Option &option, double beat, int h) {
  return h * option.speed * (player.beat - beat) / LIFETIME_BEATS + h;
}

void blit(int y, int i, Points &buffer) {
  int x = 8 * i;
  mvaddstr(y, x, "[######]");
  buffer.push_back({ x, y });
}

void drawBar(int y1, int y2, int i, Points &buffer) {
  int x = 8 * i;
  int b = (y2 < 0) ? 0 : y2 + 1;
  for (int j = b; j < y1; j++) {
    if ((j - y2) & 1) mvaddstr(j, x, " |    | ");
    else              mvaddstr(j, x, " |####| ");
    buffer.push_back({ x, j });
  }
}

void judge(Player &player, Option &option, Score &score, Lane &lane, int index) {
  if (player.judges[index]) {
    judgeln(player, option, score, lane, index);
    return;
  }

  double time = (lane.begin->beat - player.beat) * 60 / player.bpm;
  if (time >= JUDGE_BORDER_GOOD) return;

  if (time <= -JUDGE_BORDER_GOOD) {
    calcReset(score);
    lane.begin++;
    return;
  }

  int ch = lane.begin->value;
  Mix_PlayChannel(ch, player.chunkTable.at(ch), 0);

  int    judge = 0;
  double abs   = (time < 0) ? -time : time;
  if (abs < JUDGE_BORDER_COOL ) judge = 1; else
  if (abs < JUDGE_BORDER_GREAT) judge = 2; else
  if (abs < JUDGE_BORDER_GOOD ) judge = 3; else return;

  if (lane.begin->beat2 > 0) {
    player.judges[index] = judge;
    return;
  }

  calculate(score, judge);
  lane.begin++;
}

void judgeln(Player &player, Option &option, Score &score, Lane &lane, int index) {
  if (!option.autoPlay && !player.inputs[index]) {
    calcReset(score);
    player.judges[index] = 0;
    lane.begin++;
    return;
  }

  double time = (lane.begin->beat2 - player.beat) * 60 / player.bpm;
  if (time > 0) return;

  calculate(score, player.judges[index]);
  player.inputs[index] = 0;
  player.judges[index] = 0;
  lane.begin++;
}

void calculate(Score &score, int judge) {
  score.judges[judge - 1]++;
  score.combo++;
  score.totalJudges++;
}

void calcReset(Score &score) {
  score.judges[JUDGE_PHASES - 1]++;
  comboCount(score);
  score.totalJudges++;
}

void comboCount(Score &score) {
  if (score.maxCombo < score.combo)
    score.maxCombo = score.combo;
  comboBonus(score);
  score.combo = 0;
}

void comboBonus(Score &score) {
  int c = score.combo;
  int p = (score.combo - 11 < 0) ? -(score.combo - 11) : (score.combo - 11);
  int t = score.totalNotes;
  score.comboBonus = 1250 * (c * c - (c - 10) * p + 19 * c - 110) / (2 * t - 11);
}

void scoreCount(Score &score) {
  int *j = score.judges;
  int  t = score.totalNotes;
  int  c = score.comboBonus;
  score.point = (75000 * j[0] / t) + ((50000 * j[1] + 10000 * j[2]) / t) + c;
}

void handler(Player &player, Option &option, Score &score) {
  for (int i = 0; i < LANES_COUNT; i++)
    player.inputs[i] <<= 1;

  int    input = getch();
  double speed = 0;
  switch (input) {
  case '3':
    speed = option.speed - 0.25;
    if (speed < 1.00) speed = 1.00;
    option.speed = speed;
    return;

  case '4':
    speed = option.speed + 0.25;
    if (speed > MAX_SPEED) speed = MAX_SPEED;
    option.speed = speed;
    return;

  case 'q':
    player.quit = 1;
    return;

  case 'l' & 0x1f:
    clear();
    return;
  }

  if (option.autoPlay) return;

  for (int i = 0; i < LANES_COUNT; i++) {
    if (input != option.keyBinds[i]) continue;
    player.inputs[i] |= 1;
    Lane &lane = player.lanes[i];
    judge(player, option, score, lane, i);
    return;
  }
}

void gameOver(Player &player, Score &score) {
  comboCount(score);
  scoreCount(score);
  player.gameover = 1;
  player.quit     = 1;
}

void printScore(Player &player, Option &option, Score &score) {
  if (!player.gameover || option.autoPlay) return;
  std::printf("%s  %d-%d-%d-%d:%d Score:%d\n",
              player.title.c_str(),
              score.judges[0],
              score.judges[1],
              score.judges[2],
              score.judges[3],
              score.maxCombo,
              score.point);
}

void printHelp() {
  std::printf("Shinonome -- A console-based BMS player.\n"
              "Copyright (C) 2015  Kazumi Moriya <kuroclef@gmail.com>\n\n"
              "Usage:\n"
              "  shinonome [options...] [bmsfile]\n\n"
              "Options:\n"
              "  -s number Set a scroll Speed, 1.00 - 5.00\n"
              "  -k string Set the Keybindings (e.g. 'azsxdcfv')\n"
              "  -a        Enable the Autoplay mode\n"
              "  -h        Print this Help message\n\n"
              "Keybindings:\n"
              "  azsxdcfv  Hit the keys\n"
              "  34        Change the scroll speed\n"
              "  q         Quit the game\n");
  std::exit(EXIT_SUCCESS);
}
