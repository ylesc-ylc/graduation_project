#include <ncurses.h>
#include <menu.h>
#include <panel.h>
#include <form.h>
#include <string>
#include <iostream>
#include <vector>
#include <fstream>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <algorithm>
#include <unordered_set>
#include <unistd.h>
#include <filesystem>
#include <cstring>
#include <git2.h>
#include <stdexcept>
#include <sys/wait.h>
#include <git2/sys/transport.h>
#include <git2/sys/credential.h>

#define CHECK_PATH "/tmp/shellcheck_results.txt"

// class

// 文件显示类
class FileDisplay
{
private:
    // Shell关键字集合
    const std::unordered_set<std::string> SHELL_KEYWORDS = {
        "if", "then", "else", "elif", "fi", "case", "esac", "for",
        "while", "until", "do", "done", "in", "function", "select"};

    // 定义语法高亮类型
    enum class HighlightType
    {
        NORMAL,
        KEYWORD,
        STRING,
        COMMENT,
        NUMBER,
        VARIABLE,
        SYMBOL
    };

    WINDOW *win;
    std::string filename;
    std::vector<std::string> originalLines;
    std::vector<std::vector<HighlightType>> highlightInfo;
    std::vector<std::pair<std::string, std::vector<HighlightType>>> wrappedLines;
    int topLine;
    int winHeight;
    int winWidth;

public:
    FileDisplay(WINDOW *window, const std::string &file)
        : filename(file), topLine(0)
    {
        int h, w;
        getmaxyx(window, h, w);
        win = derwin(window, h - 2, w - 2, 1, 1);
        getmaxyx(win, winHeight, winWidth);
        initializeColors();
        loadFile(filename);
        analyzeSyntax();
        rewrapLines();
    }

    ~FileDisplay()
    {
        delwin(win);
    }

    // 初始化颜色
    void initializeColors()
    {
        if (has_colors())
        {
            start_color();
            init_pair(1, COLOR_GREEN, COLOR_BLACK);   // 关键字
            init_pair(2, COLOR_WHITE, COLOR_BLACK);   // 字符串
            init_pair(3, COLOR_CYAN, COLOR_BLACK);    // 注释
            init_pair(4, COLOR_MAGENTA, COLOR_BLACK); // 数字
            init_pair(5, COLOR_BLUE, COLOR_BLACK);    // 变量
            init_pair(6, COLOR_YELLOW, COLOR_BLACK);  // 符号颜色
        }
    }

    // 加载文件
    bool loadFile(const std::string &filename)
    {
        std::ifstream file(filename);
        if (!file.is_open())
        {
            return false;
        }

        originalLines.clear();
        std::string line;
        while (std::getline(file, line))
        {
            originalLines.push_back(line);
        }
        file.close();
        return true;
    }

    // 分析语法高亮
    void analyzeSyntax()
    {
        highlightInfo.clear();
        for (const auto &line : originalLines)
        {
            std::vector<HighlightType> lineInfo(line.length(), HighlightType::NORMAL);
            bool inString = false;
            bool inComment = false;
            bool escapeChar = false;

            for (size_t i = 0; i < line.length(); ++i)
            {
                if (inComment)
                {
                    lineInfo[i] = HighlightType::COMMENT;
                    continue;
                }

                if (escapeChar)
                {
                    escapeChar = false;
                    continue;
                }

                if (line[i] == '\\')
                {
                    escapeChar = true;
                    continue;
                }

                if (line[i] == '"' || line[i] == '\'')
                {
                    inString = !inString;
                    lineInfo[i] = HighlightType::STRING;
                    continue;
                }

                if (!inString && line[i] == '#')
                {
                    inComment = true;
                    lineInfo[i] = HighlightType::COMMENT;
                    continue;
                }

                if (inString)
                {
                    lineInfo[i] = HighlightType::STRING;
                }
                else if (isdigit(line[i]))
                {
                    lineInfo[i] = HighlightType::NUMBER;
                }
                else if (line[i] == '$')
                {
                    lineInfo[i] = HighlightType::VARIABLE;
                    // 变量名部分也标记为变量
                    size_t j = i + 1;
                    while (j < line.length() && (isalnum(line[j]) || line[j] == '_'))
                    {
                        lineInfo[j] = HighlightType::VARIABLE;
                        j++;
                    }
                    i = j - 1;
                }
            }
            highlightInfo.push_back(lineInfo);
        }

        // 识别关键字
        for (size_t lineNum = 0; lineNum < originalLines.size(); ++lineNum)
        {
            const auto &line = originalLines[lineNum];
            auto &info = highlightInfo[lineNum];

            size_t pos = 0;
            while (pos < line.length())
            {
                // 跳过空白和已标记部分
                while (pos < line.length() && (isspace(line[pos]) || info[pos] != HighlightType::NORMAL))
                {
                    pos++;
                }

                if (pos >= line.length())
                    break;

                // 提取单词
                size_t wordStart = pos;
                while (pos < line.length() && !isspace(line[pos]) && info[pos] == HighlightType::NORMAL)
                {
                    pos++;
                }

                std::string word = line.substr(wordStart, pos - wordStart);
                if (SHELL_KEYWORDS.find(word) != SHELL_KEYWORDS.end())
                {
                    for (size_t i = wordStart; i < wordStart + word.length(); ++i)
                    {
                        info[i] = HighlightType::KEYWORD;
                    }
                }
            }
            // 识别符号
            for (size_t i = 0; i < line.length(); ++i)
            {
                if (info[i] == HighlightType::NORMAL &&
                    (line[i] == '=' || line[i] == '+' || line[i] == '-' ||
                     line[i] == '*' || line[i] == '/' || line[i] == '|' ||
                     line[i] == '&' || line[i] == '<' || line[i] == '>' ||
                     line[i] == '(' || line[i] == ')' || line[i] == '[' ||
                     line[i] == ']' || line[i] == '{' || line[i] == '}' ||
                     line[i] == ';' || line[i] == ':'))
                {
                    info[i] = HighlightType::SYMBOL;
                }
            }
        }
    }

    // 重新计算换行
    void rewrapLines()
    {
        wrappedLines.clear();

        for (size_t lineNum = 0; lineNum < originalLines.size(); ++lineNum)
        {
            const auto &line = originalLines[lineNum];
            const auto &info = highlightInfo[lineNum];

            if (line.empty())
            {
                wrappedLines.emplace_back("", std::vector<HighlightType>());
                continue;
            }

            size_t pos = 0;
            while (pos < line.length())
            {
                int chunkSize = std::min((int)(line.length() - pos), winWidth);
                wrappedLines.emplace_back(
                    line.substr(pos, chunkSize),
                    std::vector<HighlightType>(info.begin() + pos, info.begin() + pos + chunkSize));
                pos += chunkSize;
            }
        }
    }

    // 刷新显示
    void refreshDisplay()
    {
        werase(win);
        int linesToShow = std::min(winHeight, (int)wrappedLines.size() - topLine);

        for (int i = 0; i < linesToShow; ++i)
        {
            const auto &line = wrappedLines[topLine + i].first;
            const auto &info = wrappedLines[topLine + i].second;

            for (size_t j = 0; j < line.length(); ++j)
            {
                if (j >= info.size())
                {
                    waddch(win, line[j]);
                    continue;
                }

                switch (info[j])
                {
                case HighlightType::KEYWORD:
                    wattron(win, COLOR_PAIR(1));
                    waddch(win, line[j]);
                    wattroff(win, COLOR_PAIR(1));
                    break;
                case HighlightType::STRING:
                    wattron(win, COLOR_PAIR(2));
                    waddch(win, line[j]);
                    wattroff(win, COLOR_PAIR(2));
                    break;
                case HighlightType::COMMENT:
                    wattron(win, COLOR_PAIR(3));
                    waddch(win, line[j]);
                    wattroff(win, COLOR_PAIR(3));
                    break;
                case HighlightType::NUMBER:
                    wattron(win, COLOR_PAIR(4));
                    waddch(win, line[j]);
                    wattroff(win, COLOR_PAIR(4));
                    break;
                case HighlightType::VARIABLE:
                    wattron(win, COLOR_PAIR(5));
                    waddch(win, line[j]);
                    wattroff(win, COLOR_PAIR(5));
                    break;
                case HighlightType::SYMBOL:
                    wattron(win, COLOR_PAIR(6));
                    waddch(win, line[j]);
                    wattroff(win, COLOR_PAIR(6));
                    break;
                default:
                    waddch(win, line[j]);
                    break;
                }
            }
            // waddch(win, '\n');
            int remaining = winWidth - line.length();
            if (remaining > 0)
            {
                wmove(win, i, line.length());
                for (int k = 0; k < remaining; ++k)
                {
                    waddch(win, ' ');
                }
            }
        }

        for (int i = linesToShow; i < winHeight - 1; ++i)
        {
            wmove(win, i, 0);
            for (int j = 0; j < winWidth; ++j)
            {
                waddch(win, ' ');
            }
        }

        // 显示状态信息
        if (!wrappedLines.empty())
        {
            std::string status = std::to_string(topLine + 1) + "/" +
                                 std::to_string(wrappedLines.size());
            mvwaddstr(win, winHeight - 1, winWidth - status.length() - 1, status.c_str());
        }

        wrefresh(win);
    }

    // 处理输入
    bool handleInput(int ch)
    {
        refreshDisplay();
        switch (ch)
        {
        case KEY_UP:
            if (topLine > 0)
                topLine--;
            return true;
        case KEY_DOWN:
            if (topLine < (int)wrappedLines.size() - winHeight)
                topLine++;
            return true;
        case KEY_PPAGE: // Page Up
            topLine = std::max(0, topLine - winHeight);
            return true;
        case KEY_NPAGE: // Page Down
            topLine = std::min((int)wrappedLines.size() - winHeight, topLine + winHeight);
            return true;
        default:
            return true;
        }
    }

    void run()
    {
        refreshDisplay();
    }
    bool reloadFile()
    {
        if (!loadFile(filename))
            return false; // 加载失败直接返回
        analyzeSyntax();
        rewrapLines();
        refreshDisplay();
        return true;
    }
    bool changeFile(const std::string &newFile)
    {
        wrefresh(win);
        werase(win);
        topLine = 0;
        filename = newFile;
        return reloadFile();
    }
};

// git操作类
class gitInterface
{
private:
    WINDOW *win;
    std::filesystem::path currentDir;
    std::vector<std::filesystem::path> items;
    int scrollPos;
    bool showCommitInput;
    std::string commitMessage;
    int cursorPos;
    bool hasInputFocus;
    int inputStartY, inputStartX;
    int inputLines;

    // 修改后的光标位置计算，正确处理换行
    void calculateCursorPosition(int &y, int &x)
    {
        int textWidth = getmaxx(win) - 4;
        int currentLineStart = 0;
        y = inputStartY;
        x = inputStartX;

        for (int i = 0; i < cursorPos; ++i)
        {
            if (commitMessage[i] == '\n' || x >= inputStartX + textWidth - 1)
            {
                y++;
                x = inputStartX;
                if (commitMessage[i] != '\n')
                {
                    x++; // 自动换行时保持一个字符位置
                }
            }
            else
            {
                x++;
            }
        }
    }

    void readDirectory()
    {
        items.clear();

        try
        {
            for (const auto &entry : std::filesystem::directory_iterator(currentDir))
            {
                std::filesystem::path name = entry.path().filename();
                if (name != "." && name != "..")
                {
                    items.push_back(name);
                }
            }

            std::sort(items.begin(), items.end(), [](const auto &a, const auto &b)
                      { return a.string() < b.string(); });
        }
        catch (const std::filesystem::filesystem_error &e)
        {
            items.push_back("无法打开目录: " + currentDir.string());
        }
    }

    void drawWindow()
    {
        werase(win);
        box(win, 0, 0);

        int winHeight = getmaxy(win);
        int winWidth = getmaxx(win);

        if (!showCommitInput)
        {
            // 文件选择模式
            int contentHeight = winHeight - 3;
            int maxItems = contentHeight - 2;

            int start = scrollPos;
            int end = std::min(scrollPos + maxItems, static_cast<int>(items.size()));

            for (int i = start; i < end; ++i)
            {
                std::string displayName = items[i].string();
                if (displayName.length() > static_cast<size_t>(winWidth - 4))
                {
                    displayName = displayName.substr(0, winWidth - 7) + "...";
                }
                mvwprintw(win, i - scrollPos + 1, 1, "%s", displayName.c_str());
            }

            if (scrollPos > 0)
            {
                mvwaddch(win, 0, winWidth - 2, ACS_UARROW);
            }
            if (scrollPos + maxItems < static_cast<int>(items.size()))
            {
                mvwaddch(win, contentHeight - 1, winWidth - 2, ACS_DARROW);
            }

            mvwprintw(win, 0, 2, "[ Git Add - %s ]", currentDir.string().c_str());

            // Next按钮
            if (!hasInputFocus)
                wattron(win, A_REVERSE);
            mvwprintw(win, winHeight - 2, winWidth - 10, "[ Next ]");
            if (!hasInputFocus)
                wattroff(win, A_REVERSE);
        }
        else
        {
            // 提交信息输入模式
            mvwprintw(win, 0, 2, "[ Git Commit ]");
            wattron(win, A_BOLD);
            mvwprintw(win, 2, 1, "Commit Message (Enter for newline):");
            wattroff(win, A_BOLD);

            inputStartY = 4;
            inputStartX = 2;
            inputLines = winHeight - 6;
            int textWidth = winWidth - 4;

            // 绘制文本内容，处理换行
            int line = 0;
            int pos = 0;
            while (line < inputLines && pos < commitMessage.length())
            {
                int start = pos;
                // 查找下一个换行符或行尾
                while (pos < commitMessage.length() &&
                       pos - start < textWidth &&
                       commitMessage[pos] != '\n')
                {
                    pos++;
                }

                std::string lineText = commitMessage.substr(start, pos - start);
                mvwprintw(win, inputStartY + line, inputStartX, "%s", lineText.c_str());

                // 清除行剩余部分
                if (lineText.length() < textWidth)
                {
                    mvwhline(win, inputStartY + line, inputStartX + lineText.length(),
                             ' ', textWidth - lineText.length());
                }

                line++;
                if (pos < commitMessage.length() && commitMessage[pos] == '\n')
                {
                    pos++; // 跳过换行符
                }
            }

            // 清除剩余行
            for (; line < inputLines; line++)
            {
                mvwhline(win, inputStartY + line, inputStartX, ' ', textWidth);
            }

            // Commit按钮
            if (!hasInputFocus)
                wattron(win, A_REVERSE);
            mvwprintw(win, winHeight - 2, winWidth - 11, "[ Commit ]");
            if (!hasInputFocus)
                wattroff(win, A_REVERSE);

            if (hasInputFocus)
            {
                int cursorY, cursorX;
                calculateCursorPosition(cursorY, cursorX);
                wmove(win, cursorY, cursorX);
            }
        }

        wrefresh(win);
    }

public:
    gitInterface(WINDOW *window, const std::string &dir = ".")
        : win(window), currentDir(dir), scrollPos(0),
          showCommitInput(false), commitMessage(""), cursorPos(0),
          hasInputFocus(true), inputStartY(0), inputStartX(0), inputLines(0)
    {
        keypad(win, TRUE);
        curs_set(0);
        readDirectory();
    }

    void reinitialize(const std::string &newDir)
    {
        currentDir = newDir;
        scrollPos = 0;
        showCommitInput = false;
        commitMessage.clear();
        cursorPos = 0;
        hasInputFocus = true;
        readDirectory();
    }

    std::string run()
    {
        drawWindow();

        int ch;
        while (true)
        {
            drawWindow();
            ch = wgetch(win);

            // 统一处理退出键
            if (ch == 'q' || ch == 'Q')
            {
                return "";
            }

            if (showCommitInput)
            {
                if (hasInputFocus)
                {
                    
                    curs_set(1);
                    refresh();
                    switch (ch)
                    {
                    case KEY_ENTER:
                    case '\n':
                        // 插入换行符
                        commitMessage.insert(cursorPos, 1, '\n');
                        cursorPos++;
                        break;
                    case KEY_LEFT:
                        if (cursorPos > 0)
                            cursorPos--;
                        break;
                    case KEY_RIGHT:
                        if (cursorPos < static_cast<int>(commitMessage.length()))
                            cursorPos++;
                        break;
                    case KEY_UP:
                    {
                        // 向上移动一行
                        int textWidth = getmaxx(win) - 4;
                        int lineStart = cursorPos;
                        while (lineStart > 0 && commitMessage[lineStart - 1] != '\n')
                        {
                            lineStart--;
                        }

                        if (lineStart > 0)
                        {
                            cursorPos = lineStart - 1;
                            int prevLineStart = cursorPos;
                            while (prevLineStart > 0 && commitMessage[prevLineStart - 1] != '\n')
                            {
                                prevLineStart--;
                            }
                            int offset = cursorPos - prevLineStart;
                            cursorPos = std::min(prevLineStart + offset, prevLineStart + textWidth - 1);
                        }
                        break;
                    }
                    case KEY_DOWN:
                    {
                        // 向下移动一行
                        int textWidth = getmaxx(win) - 4;
                        int lineStart = cursorPos;
                        while (lineStart > 0 && commitMessage[lineStart - 1] != '\n')
                        {
                            lineStart--;
                        }

                        int lineEnd = cursorPos;
                        while (lineEnd < commitMessage.length() && commitMessage[lineEnd] != '\n')
                        {
                            lineEnd++;
                        }

                        if (lineEnd < commitMessage.length())
                        {
                            int nextLineStart = lineEnd + 1;
                            int offset = cursorPos - lineStart;
                            cursorPos = std::min(nextLineStart + offset, nextLineStart + textWidth - 1);
                            if (cursorPos > commitMessage.length())
                            {
                                cursorPos = commitMessage.length();
                            }
                        }
                        break;
                    }
                    case KEY_HOME:
                    {
                        // 移动到行首
                        int lineStart = cursorPos;
                        while (lineStart > 0 && commitMessage[lineStart - 1] != '\n')
                        {
                            lineStart--;
                        }
                        cursorPos = lineStart;
                        break;
                    }
                    case KEY_END:
                    {
                        // 移动到行尾
                        int lineEnd = cursorPos;
                        while (lineEnd < commitMessage.length() && commitMessage[lineEnd] != '\n')
                        {
                            lineEnd++;
                        }
                        cursorPos = lineEnd;
                        break;
                    }
                    case '\t':
                        hasInputFocus = false;
                        break;
                    case KEY_BACKSPACE:
                    case 127:
                        if (cursorPos > 0)
                        {
                            commitMessage.erase(cursorPos - 1, 1);
                            cursorPos--;
                        }
                        break;
                    case KEY_DC:
                        if (cursorPos < static_cast<int>(commitMessage.length()))
                        {
                            commitMessage.erase(cursorPos, 1);
                        }
                        break;
                    default:
                        if (isprint(ch))
                        {
                            commitMessage.insert(cursorPos, 1, ch);
                            cursorPos++;
                        }
                        break;
                    }
                }
                else
                {
                    curs_set(0);
                    refresh();
                    switch (ch)
                    {
                    case '\t':
                        hasInputFocus = true;
                        break;
                    case KEY_ENTER:
                    case '\n':
                        if (!commitMessage.empty())
                            return commitMessage;
                        break;
                    }
                }
            }
            else
            {
                // 文件选择模式
                curs_set(0);
                switch (ch)
                {
                case KEY_UP:
                    if (scrollPos > 0)
                        scrollPos--;
                    break;
                case KEY_DOWN:
                    if (scrollPos + (getmaxy(win) - 5) < static_cast<int>(items.size()))
                        scrollPos++;
                    break;
                case '\t':
                    hasInputFocus = !hasInputFocus;
                    break;
                case KEY_ENTER:
                case '\n':
                    if (!hasInputFocus)
                    {
                        showCommitInput = true;
                        hasInputFocus = true;
                        cursorPos = commitMessage.length();
                    }
                    break;
                }
            }
        }
    }
};

// 菜单类
// 用于创建实验选择菜单和退出菜单
class menuChoice
{
private:
    WINDOW *menuWin;
    MENU *menu;
    std::vector<ITEM *> items;
    int selectedIndex;

public:
    menuChoice(WINDOW *win, const std::vector<std::string> &choices)
        : selectedIndex(0)
    {
        int h, w;
        getmaxyx(win, h, w);
        menuWin = derwin(win, h - 2, w - 2, 1, 1);
        for (auto it = choices.begin(); it != choices.end(); ++it)
        {
            items.push_back(new_item(it->c_str(), ""));
        }
        items.push_back(nullptr); // 结束标志
        menu = new_menu(items.data());
        set_menu_win(menu, win);
        set_menu_sub(menu, menuWin);
        set_menu_format(menu, 0, 0);
        set_menu_mark(menu, " * ");
        set_menu_spacing(menu, 1, 1, 1);
        set_menu_grey(menu, 1);
    }

    ~menuChoice()
    {

        unpost_menu(menu);
        free_menu(menu); // 删除菜单
        for (auto it = items.begin(); it != items.end(); ++it)
        {
            free_item(*it);
        } // 删除菜单项
        delwin(menuWin);
    }

    int run()
    {
        post_menu(menu);
        wrefresh(menuWin);
        int ch;
        while ((ch = getch()) != 10) // Enter key
        {
            switch (ch)
            {
            case KEY_UP:
                menu_driver(menu, REQ_UP_ITEM);
                break;
            case KEY_DOWN:
                menu_driver(menu, REQ_DOWN_ITEM);
                break;
            case 'q':
                return -1; // 退出
            default:
                break;
            }
            wrefresh(menuWin);
        }
        selectedIndex = item_index(current_item(menu));

        return selectedIndex;
    }
};

// 选择专业
std::string specializationChoice()
{
    curs_set(0); // 隐藏光标
    std::vector<std::string> specializationList;
    specializationList.push_back("Data Science and Big Data Technology");
    specializationList.push_back("Network Engineering");
    specializationList.push_back("Intelligent Science and Technology");
    specializationList.push_back("Cyberspace Security");

    // 创建窗口
    WINDOW *specializationWin = newwin(10, 42, (LINES - 10) / 2, (COLS - 40) / 2);
    WINDOW *specializationWin2 = derwin(specializationWin, 6, 38, 3, 1);
    // 创建子窗口
    box(specializationWin, 0, 0);
    mvwprintw(specializationWin, 0, 1, "Choose your specialization");
    // 创建菜单项
    std::vector<ITEM *> items;
    for (auto it = specializationList.begin(); it != specializationList.end(); ++it)
    {
        items.push_back(new_item(it->c_str(), ""));
    }
    items.push_back(nullptr);
    MENU *specializationMenu = new_menu(items.data());
    set_menu_win(specializationMenu, specializationWin);
    set_menu_sub(specializationMenu, specializationWin2);
    set_menu_format(specializationMenu, 6, 1);
    set_menu_mark(specializationMenu, " * ");
    set_menu_spacing(specializationMenu, 1, 1, 1);
    set_menu_grey(specializationMenu, 1);
    // 创建并设置菜单

    post_menu(specializationMenu);
    wrefresh(specializationWin);
    int c;
    while ((c = getch()) != 10)
    {
        switch (c)
        {
        case KEY_UP:
            menu_driver(specializationMenu, REQ_UP_ITEM);
            break;
        case KEY_DOWN:
            menu_driver(specializationMenu, REQ_DOWN_ITEM);
            break;
        default:
            break;
        }
        wrefresh(specializationWin);
    }
    int choice = item_index(current_item(specializationMenu));
    unpost_menu(specializationMenu);
    free_menu(specializationMenu); // 删除菜单
    for (auto it = items.begin(); it != items.end(); ++it)
    {
        free_item(*it);
    } // 删除菜单项
    delwin(specializationWin);
    delwin(specializationWin2); // 删除窗口
    // 释放内存
    return specializationList[choice];
}

// 填写学生信息
void studentInfo(nlohmann::json &student)
{
    // 显示普通光标
    curs_set(1);

    // 创建表单字段
    std::vector<FIELD *> fields;
    fields.push_back(new_field(1, 25, 2, 15, 0, 0));  // 名字
    fields.push_back(new_field(1, 25, 4, 15, 0, 0));  // 学号
    fields.push_back(new_field(1, 25, 6, 15, 0, 0));  // 班级
    fields.push_back(new_field(1, 25, 8, 15, 0, 0));  // Git仓库
    fields.push_back(new_field(1, 25, 10, 15, 0, 0)); // 仓库
    fields.push_back(new_field(1, 25, 12, 15, 0, 0)); // 邮箱
    fields.push_back(nullptr);                        // 结束标记

    // 设置字段属性
    for (auto it = fields.begin(); *it != nullptr; ++it)
    {
        set_field_back(*it, A_UNDERLINE);
        field_opts_off(*it, O_AUTOSKIP);
        set_field_type(*it, TYPE_REGEXP, "^.+$"); // 设置非空验证
    }

    // 创建表单窗口
    WINDOW *formWin = newwin(22, 60, (LINES - 20) / 2, (COLS - 60) / 2);
    WINDOW *formWin2 = derwin(formWin, 20, 50, 1, 5);
    box(formWin, 0, 0);
    mvwprintw(formWin, 0, 1, "Please fill in your information");
    keypad(formWin, TRUE);

    // 创建表单
    FORM *form = new_form(fields.data());
    set_form_win(form, formWin);
    set_form_sub(form, formWin2);
    post_form(form);
    set_current_field(form, fields.at(0)); // 设置name为当前字段

    // 显示标签
    mvwprintw(formWin, 3, 5, "name:");
    mvwprintw(formWin, 5, 5, "number:");
    mvwprintw(formWin, 7, 5, "class:");
    mvwprintw(formWin, 9, 5, "Git:");
    mvwprintw(formWin, 11, 5, "");
    mvwprintw(formWin, 13, 5, "email:");
    mvwprintw(formWin, 18, 5, "Tab to switch, Enter in Next to submit");

    // 绘制Next按钮（直接在formWin上）
    int nextBtnY = 18, nextBtnX = 45;
    mvwprintw(formWin, nextBtnY, nextBtnX, "[ Next ]");
    wrefresh(formWin);

    // 当前焦点状态 (0:表单, 1:按钮)
    int focusState = 0;

    // 表单处理循环
    int ch;
    bool running = true;
    form_driver(form, REQ_NEXT_FIELD);
    form_driver(form, REQ_PREV_FIELD);
    while (running)
    {
        if (focusState == 0)
        {
            // 表单焦点

            curs_set(1);
            ch = wgetch(formWin);
            switch (ch)
            {
            case KEY_DOWN:
                form_driver(form, REQ_NEXT_FIELD);
                form_driver(form, REQ_END_LINE);
                break;
            case KEY_UP:
                form_driver(form, REQ_PREV_FIELD);
                form_driver(form, REQ_END_LINE);
                break;
            case KEY_LEFT:
                form_driver(form, REQ_PREV_CHAR);
                break;
            case KEY_RIGHT:
                form_driver(form, REQ_NEXT_CHAR);
                break;
            case KEY_BACKSPACE:
            case 127:
                form_driver(form, REQ_DEL_PREV);
                break;
            case '\t':
                // 切换到按钮
                focusState = 1;
                curs_set(0);
                // 高亮显示按钮
                wattron(formWin, A_REVERSE);
                mvwprintw(formWin, nextBtnY, nextBtnX, "[ Next ]");
                wattroff(formWin, A_REVERSE);
                wrefresh(formWin);
                break;
            case '\n':
                // 在表单内回车，切换到下一个字段
                form_driver(form, REQ_NEXT_FIELD);
                form_driver(form, REQ_END_LINE);
                break;
            default:
                form_driver(form, ch);
                break;
            }
        }
        else
        {
            // 按钮焦点
            ch = wgetch(formWin);
            curs_set(0);
            switch (ch)
            {
            case '\t':
                // 切换回表单
                focusState = 0;
                // 取消按钮高亮
                wattrset(formWin, A_NORMAL);
                mvwprintw(formWin, nextBtnY, nextBtnX, "[ Next ]");
                wrefresh(formWin);
                form_driver(form, REQ_NEXT_FIELD);
                form_driver(form, REQ_PREV_FIELD);
                form_driver(form, REQ_END_LINE);
                break;
            case '\n':
                // 检查所有字段是否已填写
                form_driver(form, REQ_VALIDATION);
                bool allFilled = true;
                for (auto it = fields.begin(); *it != nullptr; ++it)
                {
                    if (field_buffer(*it, 0)[0] == ' ')
                    {
                        allFilled = false;
                        break;
                    }
                }
                if (allFilled)
                {
                    running = false;
                }
                else
                {
                    mvwprintw(formWin, 20, 5, "Please fill all fields!");
                    wrefresh(formWin);
                    // 切换回表单第一个字段
                    focusState = 0;
                    wattrset(formWin, A_NORMAL);
                    mvwprintw(formWin, nextBtnY, nextBtnX, "[ Next ]");
                    set_current_field(form, fields.at(0));
                    wrefresh(formWin);
                    form_driver(form, REQ_NEXT_FIELD);
                    form_driver(form, REQ_PREV_FIELD);
                    form_driver(form, REQ_END_LINE);
                }
                break;
            }
        }
    }
    // 获取字段值
    std::string name = field_buffer(fields.at(0), 0);
    std::string number = field_buffer(fields.at(1), 0);
    std::string className = field_buffer(fields.at(2), 0);
    std::string git = field_buffer(fields.at(3), 0);
    std::string git2 = field_buffer(fields.at(4), 0);
    std::string email = field_buffer(fields.at(5), 0);
    // 格式化字符串
    name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
    number.erase(std::remove(number.begin(), number.end(), ' '), number.end());
    className.erase(std::remove(className.begin(), className.end(), ' '), className.end());
    git.erase(std::remove(git.begin(), git.end(), ' '), git.end());
    git2.erase(std::remove(git2.begin(), git2.end(), ' '), git2.end());
    email.erase(std::remove(email.begin(), email.end(), ' '), email.end());
    // 将信息存入json
    student["name"] = name;
    student["number"] = number;
    student["class"] = className;
    student["git"] = git + git2;
    student["email"] = email;

    // 清理
    unpost_form(form);
    free_form(form);
    for (auto it = fields.begin(); *it != nullptr; ++it)
    {
        free_field(*it);
    }
    delwin(formWin2);
    delwin(formWin);
    clear();
    refresh();
}

// 编辑器选择
std::string editorChoice()
{
    curs_set(0); // 隐藏光标
    std::vector<std::string> editorList;
    editorList.push_back("vim");
    editorList.push_back("nano");
    editorList.push_back("emacs");
    editorList.push_back("neovim");

    // 创建窗口
    WINDOW *editorWin = newwin(10, 42, (LINES - 10) / 2, (COLS - 40) / 2);
    WINDOW *editorWin2 = derwin(editorWin, 6, 38, 3, 1);
    // 创建子窗口
    box(editorWin, 0, 0);
    mvwprintw(editorWin, 0, 1, "Choose your editor");
    // 创建菜单项
    std::vector<ITEM *> items;
    for (auto it = editorList.begin(); it != editorList.end(); ++it)
    {
        items.push_back(new_item(it->c_str(), ""));
    }
    items.push_back(nullptr);
    MENU *editorMenu = new_menu(items.data());
    set_menu_win(editorMenu, editorWin);
    set_menu_sub(editorMenu, editorWin2);
    set_menu_format(editorMenu, 6, 1);
    set_menu_mark(editorMenu, " * ");
    set_menu_spacing(editorMenu, 1, 1, 1);
    set_menu_grey(editorMenu, 1);
    // 创建并设置菜单

    post_menu(editorMenu);
    wrefresh(editorWin);
    int c;
    while ((c = getch()) != 10)
    {
        switch (c)
        {
        case KEY_UP:
            menu_driver(editorMenu, REQ_UP_ITEM);
            break;
        case KEY_DOWN:
            menu_driver(editorMenu, REQ_DOWN_ITEM);
            break;
        default:
            break;
        }
        wrefresh(editorWin);
    }
    int choice = item_index(current_item(editorMenu));
    unpost_menu(editorMenu);
    free_menu(editorMenu); // 删除菜单
    for (auto it = items.begin(); it != items.end(); ++it)
    {
        free_item(*it);
    }
    // 删除菜单项
    delwin(editorWin);
    delwin(editorWin2);
    // 删除窗口
    clear();
    refresh();
    return editorList[choice];
    // 返回编辑器名称
}

// 调用asciinema记录编辑器
/**
 * editor: 编辑器
 * filename: 文件名
 * recordFile: 记录文件名
 */
void record_with_asciinema(const std::string &editor,
                           const std::string &filename,
                           const std::string &recording_file)
{
    // Save current ncurses state
    def_prog_mode();
    // End ncurses mode and return to normal terminal mode
    endwin();

    // Construct the command string
    std::string command = editor + " " + filename;

    // Fork a child process
    pid_t pid = fork();
    if (pid == -1)
    {
        // Fork failed
        reset_prog_mode();
        refresh();
        throw std::runtime_error("Failed to fork process");
    }
    else if (pid == 0)
    {
        // Child process - execute asciinema
        execlp("asciinema",
               "asciinema",
               "rec",
               "-q",
               "--stdin",
               "--overwrite",
               "--command",
               command.c_str(),
               recording_file.c_str(),
               nullptr);

        // If execlp returns, it failed
        exit(EXIT_FAILURE);
    }
    else
    {
        // Parent process - wait for child to complete
        int status;
        waitpid(pid, &status, 0);

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            reset_prog_mode();
            refresh();
            throw std::runtime_error("asciinema recording failed");
        }
    }

    // Restore ncurses state
    reset_prog_mode();
    // Reinitialize ncurses
    refresh();
}

void runShellCheck(const std::string &scriptPath)
{
    // 构建ShellCheck命令
    std::string command = "shellcheck " + scriptPath + " > " + CHECK_PATH + " 2>&1";

    // 执行命令
    system(command.c_str());
}

// git操作函数

// git init
bool git_init_with_config(const std::string &working_dir,
                          const std::string &user_name,
                          const std::string &user_email,
                          bool global_config = false)
{
    // // 初始化libgit2库
    // if (git_libgit2_init() < 0)
    // {
    //     const git_error *e = giterr_last();
    //     std::cerr << "Failed to initialize libgit2: "
    //               << (e ? e->message : "Unknown error") << '\n';
    //     return false;
    // }

    // 初始化Git仓库
    git_repository *repo = nullptr;
    int result = git_repository_init(&repo, working_dir.c_str(), 0);

    if (result >= 0)
    {
        // 打开配置（仓库级或全局）
        git_config *cfg = nullptr;
        if (global_config ? git_config_open_default(&cfg) >= 0
                          : git_repository_config(&cfg, repo) >= 0)
        {
            // 设置用户名称
            if (git_config_set_string(cfg, "user.name", user_name.c_str()) < 0)
            {
                const git_error *e = giterr_last();
                std::cerr << "Failed to set user.name: "
                          << (e ? e->message : "Unknown error") << '\n';
                result = -1;
            }

            // 设置用户邮箱
            if (result >= 0 && git_config_set_string(cfg, "user.email", user_email.c_str()) < 0)
            {
                const git_error *e = giterr_last();
                std::cerr << "Failed to set user.email: "
                          << (e ? e->message : "Unknown error") << '\n';
                result = -1;
            }

            git_config_free(cfg);
        }
        else
        {
            const git_error *e = giterr_last();
            std::cerr << "Failed to open config: "
                      << (e ? e->message : "Unknown error") << '\n';
            result = -1;
        }
    }

    // 清理资源
    if (repo)
        git_repository_free(repo);
    //git_libgit2_shutdown();

    // 处理结果
    if (result < 0)
    {
        return false;
    }

    // std::cout << "Initialized empty Git repository in " << working_dir << '\n';
    // std::cout << "Configured user.name = " << user_name
    //           << (global_config ? " (global)" : "") << '\n';
    // std::cout << "Configured user.email = " << user_email
    //           << (global_config ? " (global)" : "") << '\n';
    return true;
}

// git add
bool git_add_all(const std::string &repo_path)
{
    // // 初始化libgit2库
    // if (git_libgit2_init() < 0)
    // {
    //     const git_error *e = giterr_last();
    //     std::cerr << "Failed to initialize libgit2: "
    //               << (e ? e->message : "Unknown error") << '\n';
    //     return false;
    // }

    git_repository *repo = nullptr;
    git_index *index = nullptr;
    int result = 0;

    // 打开仓库
    if (git_repository_open(&repo, repo_path.c_str()) < 0)
    {
        const git_error *e = giterr_last();
        std::cerr << "Failed to open repository: "
                  << (e ? e->message : "Unknown error") << '\n';
        result = -1;
        goto cleanup;
    }

    // 获取仓库索引
    if (git_repository_index(&index, repo) < 0)
    {
        const git_error *e = giterr_last();
        std::cerr << "Failed to get repository index: "
                  << (e ? e->message : "Unknown error") << '\n';
        result = -1;
        goto cleanup;
    }

    // 添加所有工作目录更改到索引
    if (git_index_add_all(index, nullptr, 0, nullptr, nullptr) < 0)
    {
        const git_error *e = giterr_last();
        std::cerr << "Failed to add files to index: "
                  << (e ? e->message : "Unknown error") << '\n';
        result = -1;
        goto cleanup;
    }

    // 写入索引到磁盘
    if (git_index_write(index) < 0)
    {
        const git_error *e = giterr_last();
        std::cerr << "Failed to write index: "
                  << (e ? e->message : "Unknown error") << '\n';
        result = -1;
        goto cleanup;
    }

    // std::cout << "Added all changes to the staging area.\n";

cleanup:
    // 清理资源
    if (index)
        git_index_free(index);
    if (repo)
        git_repository_free(repo);
    //git_libgit2_shutdown();

    return result == 0;
}

// git commit
bool use_git_commit(const std::string &repo_path, const std::string &message)
{
    // 初始化libgit2库
    //git_libgit2_init();

    // 声明所有需要释放的资源
    git_repository *repo = nullptr;
    git_index *index = nullptr;
    git_signature *signature = nullptr;
    git_tree *tree = nullptr;
    git_commit *parent_commit = nullptr;
    bool success = false;

    try
    {
        // 1. 打开Git仓库
        if (git_repository_open(&repo, repo_path.c_str()) < 0)
        {
            throw std::runtime_error("无法打开仓库: " + std::string(giterr_last()->message));
        }

        // 2. 获取并刷新索引
        if (git_repository_index(&index, repo) < 0)
        {
            throw std::runtime_error("无法获取索引: " + std::string(giterr_last()->message));
        }
        if (git_index_read(index, 1) < 0)
        { // 强制刷新索引
            throw std::runtime_error("无法刷新索引: " + std::string(giterr_last()->message));
        }

        // 3. 检查是否有待提交的更改
        if (git_index_entrycount(index) == 0)
        {
            throw std::runtime_error("没有待提交的更改");
        }

        // 4. 创建树对象
        git_oid tree_id;
        if (git_index_write_tree(&tree_id, index) < 0)
        {
            throw std::runtime_error("无法写入树对象: " + std::string(giterr_last()->message));
        }
        if (git_tree_lookup(&tree, repo, &tree_id) < 0)
        {
            throw std::runtime_error("无法查找树对象: " + std::string(giterr_last()->message));
        }

        // 5. 获取父提交(如果不是首次提交)
        git_oid parent_id;
        const git_commit *parents[1] = {nullptr};
        int parent_count = 0;

        if (git_reference_name_to_id(&parent_id, repo, "HEAD") == 0)
        {
            if (git_commit_lookup(&parent_commit, repo, &parent_id) < 0)
            {
                throw std::runtime_error("无法查找父提交: " + std::string(giterr_last()->message));
            }
            parents[0] = parent_commit;
            parent_count = 1;
        }

        // 6. 创建签名(带多重回退机制)
        if (git_signature_default(&signature, repo) < 0)
        {
            // 尝试从环境变量获取
            const char *name = std::getenv("GIT_AUTHOR_NAME");
            const char *email = std::getenv("GIT_AUTHOR_EMAIL");
            if (!name || !email || git_signature_now(&signature, name, email) < 0)
            {
                // 最终回退方案
                if (git_signature_now(&signature, "Git User", "user@example.com") < 0)
                {
                    throw std::runtime_error("无法创建签名: " + std::string(giterr_last()->message));
                }
            }
        }

        // 7. 创建提交
        git_oid commit_id;
        if (git_commit_create(
                &commit_id,
                repo,
                "HEAD",
                signature,
                signature,
                "UTF-8",
                message.c_str(),
                tree,
                parent_count,
                parent_count ? parents : nullptr) < 0)
        {
            throw std::runtime_error("无法创建提交: " + std::string(giterr_last()->message));
        }

        // std::cout << "提交成功: " << git_oid_tostr_s(&commit_id) << std::endl;
        success = true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "错误: " << e.what() << std::endl;
        if (const git_error *err = giterr_last())
        {
            std::cerr << "Git错误详情: " << err->message << std::endl;
        }
    }

    // 释放所有资源
    if (signature)
        git_signature_free(signature);
    if (tree)
        git_tree_free(tree);
    if (parent_commit)
        git_commit_free(parent_commit);
    if (index)
        git_index_free(index);
    if (repo)
        git_repository_free(repo);

    //git_libgit2_shutdown();
    return success;
}

// git remote
bool git_remote_add_origin(const std::string &repo_path, const std::string &url)
{
    // // 初始化libgit2库
    // if (git_libgit2_init() < 0)
    // {
    //     const git_error *e = giterr_last();
    //     std::cerr << "Failed to initialize libgit2: "
    //               << (e ? e->message : "Unknown error") << '\n';
    //     return false;
    // }

    git_repository *repo = nullptr;
    git_remote *remote = nullptr;
    int result = 0;

    // 打开仓库
    if (git_repository_open(&repo, repo_path.c_str()) < 0)
    {
        const git_error *e = giterr_last();
        std::cerr << "Failed to open repository: "
                  << (e ? e->message : "Unknown error") << '\n';
        result = -1;
        goto cleanup;
    }

    // 检查是否已存在名为"origin"的远程
    if (git_remote_lookup(&remote, repo, "origin") == 0)
    {
        std::cerr << "Remote 'origin' already exists\n";
        git_remote_free(remote);
        result = -1;
        goto cleanup;
    }

    // 创建新的远程
    if (git_remote_create(&remote, repo, "origin", url.c_str()) < 0)
    {
        const git_error *e = giterr_last();
        std::cerr << "Failed to create remote: "
                  << (e ? e->message : "Unknown error") << '\n';
        result = -1;
        goto cleanup;
    }

    // std::cout << "Added remote 'origin' with URL: " << url << '\n';

cleanup:
    // 清理资源
    if (remote)
        git_remote_free(remote);
    if (repo)
        git_repository_free(repo);
    //git_libgit2_shutdown();

    return result == 0;
}

// git 远程操作
//  错误处理函数
void check_error(int error_code, const char *action)
{
    if (error_code < 0)
    {
        const git_error *error = giterr_last();
        std::cerr << "Error (" << error_code << ") during " << action << ": "
                  << (error ? error->message : "Unknown error") << std::endl;
        exit(1);
    }
}

// SSH 认证回调函数
int credentials_callback(git_cred **cred, const char *url, const char *username_from_url,
                         unsigned int allowed_types, void *payload)
{
    (void)url;
    (void)username_from_url;
    (void)payload;

    // 只处理 SSH 代理方式
    if (allowed_types & GIT_CREDENTIAL_SSH_KEY)
    {
        return git_cred_ssh_key_from_agent(cred, username_from_url);
    }

    std::cerr << "Unsupported credential type" << std::endl;
    return -1;
}

// git push
int use_git_push(const std::string &repo_path, const std::string &url)
{
    //git_libgit2_init();

    git_repository *repo = nullptr;
    int error = git_repository_open(&repo, repo_path.c_str());
    check_error(error, "opening repository");

    // 获取或创建 remote
    git_remote *remote = nullptr;
    error = git_remote_lookup(&remote, repo, "origin");
    if (error < 0)
    {
        std::cout << "Remote 'origin' not found, creating it\n";
        error = git_remote_create(&remote, repo, "origin", url.c_str());
        check_error(error, "creating remote");
        check_error(error, "saving remote config");
    }

    // 设置 push 选项和回调
    git_push_options push_options;
    git_push_options_init(&push_options, GIT_PUSH_OPTIONS_VERSION);

    git_remote_callbacks callbacks = GIT_REMOTE_CALLBACKS_INIT;
    callbacks.credentials = credentials_callback;
    push_options.callbacks = callbacks;

    // 设置 refspecs（本地 master 推送到远程 master）
    const char *refspec = "refs/heads/master:refs/heads/master";
    const git_strarray refspecs = {
        (char **)&refspec,
        1};

    // 执行 push（包含自动连接）
    error = git_remote_push(remote, &refspecs, &push_options);
    check_error(error, "pushing to remote");

    // 设置 upstream 分支
    git_reference *local_ref = nullptr;
    error = git_branch_lookup(&local_ref, repo, "master", GIT_BRANCH_LOCAL);
    check_error(error, "looking up local branch");

    error = git_branch_set_upstream(local_ref, "origin/master");
    check_error(error, "setting upstream branch");

    // std::cout << "Successfully pushed to origin/master and set upstream" << std::endl;

    git_reference_free(local_ref);
    git_remote_free(remote);
    git_repository_free(repo);
    //git_libgit2_shutdown();

    return 0;
}

// git pull
int use_git_pull(std::string &repo_path)
{
    git_libgit2_init();

    git_repository *repo = nullptr;
    git_remote *remote = nullptr;
    git_reference *remote_ref = nullptr;
    git_reference *local_ref = nullptr;
    git_annotated_commit *annotated_commit = nullptr;
    git_tree *tree = nullptr;
    git_signature *signature = nullptr;
    git_commit *local_commit = nullptr;
    git_commit *remote_commit = nullptr;

    int error = git_repository_open(&repo, repo_path.c_str());
    check_error(error, "opening repository");

    // 获取远程
    error = git_remote_lookup(&remote, repo, "origin");
    check_error(error, "looking up remote 'origin'");

    // 设置 fetch 选项
    git_fetch_options fetch_options = GIT_FETCH_OPTIONS_INIT;
    git_remote_callbacks callbacks = GIT_REMOTE_CALLBACKS_INIT;
    callbacks.credentials = credentials_callback;
    fetch_options.callbacks = callbacks;

    // 从远程获取更新
    error = git_remote_fetch(remote, NULL, &fetch_options, "fetch");
    check_error(error, "fetching from remote");

    // 获取远程分支引用
    error = git_branch_lookup(&remote_ref, repo, "origin/master", GIT_BRANCH_REMOTE);
    check_error(error, "looking up remote branch");

    // 获取本地当前分支引用
    error = git_repository_head(&local_ref, repo);
    check_error(error, "getting local branch");

    // 获取注释提交
    error = git_annotated_commit_from_ref(&annotated_commit, repo, remote_ref);
    check_error(error, "getting annotated commit");

    // 检查是否需要合并
    git_merge_analysis_t analysis;
    git_merge_preference_t preference;
    error = git_merge_analysis(&analysis, &preference, repo,
                               (const git_annotated_commit **)&annotated_commit, 1);
    check_error(error, "merge analysis");

    if (!(analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE))
    {
        // 设置合并和检出选项
        git_checkout_options checkout_options = GIT_CHECKOUT_OPTIONS_INIT;
        checkout_options.checkout_strategy = GIT_CHECKOUT_SAFE | GIT_CHECKOUT_RECREATE_MISSING;

        git_merge_options merge_options = GIT_MERGE_OPTIONS_INIT;

        // 执行合并
        error = git_merge(repo, (const git_annotated_commit **)&annotated_commit, 1,
                          &merge_options, &checkout_options);

        if (error != GIT_ECONFLICT)
        {
            check_error(error, "merging");

            // 检查索引是否有未解决的冲突
            git_index *index = nullptr;
            error = git_repository_index(&index, repo);
            check_error(error, "getting repository index");

            bool has_conflicts = git_index_has_conflicts(index);
            git_index_free(index);

            if (!has_conflicts)
            {
                // 创建合并提交
                git_oid new_commit_id;
                error = git_signature_default(&signature, repo);
                check_error(error, "creating signature");

                error = git_repository_index(&index, repo);
                check_error(error, "getting repository index");

                error = git_index_write_tree(&new_commit_id, index);
                check_error(error, "writing tree");
                git_index_free(index);

                error = git_tree_lookup(&tree, repo, &new_commit_id);
                check_error(error, "looking up tree");

                error = git_reference_peel((git_object **)&local_commit, local_ref, GIT_OBJ_COMMIT);
                check_error(error, "peeling local reference");

                error = git_reference_peel((git_object **)&remote_commit, remote_ref, GIT_OBJ_COMMIT);
                check_error(error, "peeling remote reference");

                git_commit *parents[] = {local_commit, remote_commit};

                error = git_commit_create(&new_commit_id, repo, "HEAD", signature, signature,
                                          NULL, "Merge branch 'origin/master'", tree,
                                          2, (const git_commit **)parents);
                check_error(error, "creating merge commit");
            }
            else
            {
                std::cerr << "There are unresolved conflicts. Aborting." << std::endl;
            }
        }
        else
        {
            std::cerr << "Merge conflicts detected. Please resolve them manually." << std::endl;
        }
    }

    // 清理资源
    git_tree_free(tree);
    git_signature_free(signature);
    git_commit_free(local_commit);
    git_commit_free(remote_commit);
    git_annotated_commit_free(annotated_commit);
    git_reference_free(remote_ref);
    git_reference_free(local_ref);
    git_remote_free(remote);
    git_repository_free(repo);
    git_libgit2_shutdown();

    return 0;
}

// 初始化
void init(std::filesystem::path &workDir)
{
    nlohmann::json student;
    std::string specialization = specializationChoice();
    student["specialization"] = specialization;
    studentInfo(student);
    std::string editor = editorChoice();
    student["editor"] = editor;
    student["lab_sh"] = {"lab1.sh", "lab2.sh", "lab3.sh", "lab4.sh"};
    student["lab_dir"] = {"lab1", "lab2", "lab3", "lab4"};
    student["lab1"] = nlohmann::json::array();
    student["lab2"] = nlohmann::json::array();
    student["lab3"] = nlohmann::json::array();
    student["lab4"] = nlohmann::json::array();
    std::ofstream outFile(workDir / "student.json");
    outFile << student.dump(4);
    outFile.close();
    std::ofstream initFile(workDir / "README.md");
    initFile.close();
    for (int i = 0; i < student["lab_dir"].size(); i++)
    {
        std::filesystem::create_directories(workDir / student["lab_dir"][i]);
        std::ofstream outfile(workDir / student["lab_dir"][i] / student["lab_sh"][i]);
    }
    std::filesystem::create_directories(workDir / "Require");

    std::string workDirStr = workDir.string();
    std::string name = student["name"];
    std::string email = student["email"];
    std::string url = student["git"];

    git_init_with_config(workDirStr, name, email);
    git_add_all(workDirStr);
    use_git_commit(workDirStr, "init");
    git_remote_add_origin(workDirStr, url);
    use_git_push(workDirStr, url);
}

// welcome
void welcome()
{
    const char *WELCOME_TEXT =
        R"(
__        _______ _     ____ ___  __  __ _____ 
\ \      / / ____| |   / ___/ _ \|  \/  | ____|
 \ \ /\ / /|  _| | |  | |  | | | | |\/| |  _|  
  \ V  V / | |___| |__| |__| |_| | |  | | |___ 
   \_/\_/  |_____|_____\____\___/|_|  |_|_____|
)";

    int rows, cols;

    getmaxyx(stdscr, rows, cols);
    WINDOW *start_win = newwin(0, 0, 0, 0);
    box(start_win, 0, 0);

    int line_count = 0;
    int max_width = 0;
    size_t start = 0, end;
    std::string text = WELCOME_TEXT;

    // 计算最大宽度和行数
    while ((end = text.find('\n', start)) != std::string::npos)
    {
        max_width = std::max(max_width, static_cast<int>(end - start));
        line_count++;
        start = end + 1;
    }

    int x = (cols - max_width) / 2;
    int y = (rows - line_count) / 2;

    // 逐行输出居中文本
    start = 0;
    int current_line = 0;
    while ((end = text.find('\n', start)) != std::string::npos)
    {
        mvwprintw(start_win, (y + current_line), x, text.substr(start, end - start).c_str());
        start = end + 1;
        current_line++;
    }
    wrefresh(start_win);
    napms(2000);
    delwin(start_win);
    clear();
    refresh();
}

// 初始选择实验
std::string lab_choice(nlohmann::json &student)
{
    std::vector<std::string> dir;
    for (int i = 0; i < student["lab_dir"].size(); i++)
    {
        dir.push_back(student["lab_dir"][i]);
    }
    WINDOW *labWin = newwin(8, 25, (LINES - 8) / 2, (COLS - 25) / 2);
    box(labWin, 0, 0);
    mvwprintw(labWin, 0, 1, "lab");
    wrefresh(labWin);
    auto *labMenu = new menuChoice(labWin, dir);
    std::string lab_dir = dir[labMenu->run()];
    delete labMenu;
    delwin(labWin);
    clear();
    refresh();

    return lab_dir;
}

void mainProgram(const nlohmann::json &student, const std::filesystem::path &workDir, const std::string &lab_c)
{

    std::string lab = lab_c;               // 实验
    std::string shellFile = lab + ".sh";   // 学生的shell脚本
    std::string demandFile = lab + ".txt"; // 要求
    std::string recordFile = lab + ".cast";
    std::string gitUrl = student["git"];
    std::string editor = student["editor"];
    std::vector<std::string> dir;
    for (int i = 0; i < student["lab_dir"].size(); i++)
    {
        dir.push_back(student["lab_dir"][i]);
    }
    std::vector<std::string> exitInfo = {"exit_and_push", "exit"};
    // 创建主窗口
    WINDOW *mainWin = newwin(LINES, COLS, 0, 0);
    int height, width;
    getmaxyx(mainWin, height, width);
    // 创建主窗口的三个子窗口
    // 学生shell窗口
    WINDOW *shellWin = derwin(mainWin, height - 4, width / 2 - 1, 1, 1);
    box(shellWin, 0, 0);
    mvwprintw(shellWin, 0, 1, "Shell");
    //wrefresh(shellWin);
    // 要求窗口
    WINDOW *demandWin = derwin(mainWin, height - 4, width / 2 - 1, 1, (width / 2 + 1));
    box(demandWin, 0, 0);
    mvwprintw(demandWin, 0, 1, "Demand");
    //wrefresh(demandWin);
    // 操作提示窗口
    WINDOW *buttonWIN = derwin(mainWin, 3, width - 1, height - 3, 1);
    box(buttonWIN, 0, 0);
    mvwprintw(buttonWIN, 0, 1, "Button");
    mvwprintw(buttonWIN, 1, 1, "s:start");
    mvwprintw(buttonWIN, 1, 11, "g:git");
    mvwprintw(buttonWIN, 1, 21, "c:check");
    mvwprintw(buttonWIN, 1, 31, "l:choice lab");
    mvwprintw(buttonWIN, 1, 51, "q:exit");
    //wrefresh(buttonWIN);
    // git窗口
    WINDOW *gitWin = newwin(20, 60, (LINES - 20) / 2, (COLS - 60) / 2);
    box(gitWin, 0, 0);
    //wrefresh(gitWin);
    // shellcheck窗口
    WINDOW *checkWin = newwin(LINES - 4, 60, 1, (COLS - 60) / 2);
    box(checkWin, 0, 0);
    mvwprintw(checkWin, 0, 1, "ShellCheck Results");
    // 实验选择窗口
    WINDOW *labWin = newwin(8, 25, (LINES - 8) / 2, (COLS - 25) / 2);
    box(labWin, 0, 0);
    mvwprintw(labWin, 0, 1, "lab");
    // 退出选项窗口
    WINDOW *exitWin = newwin(8, 25, (LINES - 8) / 2, (COLS - 25) / 2); // 退出窗口
    box(exitWin, 0, 0);
    mvwprintw(exitWin, 0, 1, "Exit");
    refresh();

    // 创建面板
    PANEL *mainPanel = new_panel(mainWin);
    PANEL *gitPanel = new_panel(gitWin);
    PANEL *checkPanel = new_panel(checkWin);
    PANEL *labPanel = new_panel(labWin);
    PANEL *exitPanel = new_panel(exitWin);
    top_panel(mainPanel);
    update_panels();
    doupdate();
    // 创建主窗口显示和操作对象
    FileDisplay *shellDisplay = new FileDisplay(shellWin, workDir / lab / shellFile);
    FileDisplay *demandDisplay = new FileDisplay(demandWin, workDir / "Require" / demandFile);
    // shellcheck对象
    FileDisplay *checkDisplay = new FileDisplay(checkWin, CHECK_PATH);
    // git对象
    gitInterface *git = new gitInterface(gitWin, workDir/lab);
    // 实验选择对象
    menuChoice *labChoice = new menuChoice(labWin, dir);
    // 退出选择对象
    menuChoice *labExit = new menuChoice(exitWin, exitInfo);

    shellDisplay->run();
    demandDisplay->run();

    int ch;
    curs_set(0);
    bool run = true;
    while (run)
    {
        ch = getch();
        switch (ch)
        {
        case KEY_DOWN:
        {
            shellDisplay->handleInput(ch);
            demandDisplay->handleInput(ch);
            break;
        }

        case KEY_UP:
        {
            shellDisplay->handleInput(ch);
            demandDisplay->handleInput(ch);
            break;
        }

        case 'l':
        {
            top_panel(labPanel);
            update_panels();
            doupdate();
            int choice = labChoice->run();
            if (choice == -1)
            {
                top_panel(mainPanel);
                update_panels();
                doupdate();
            }
            else
            {
                lab = dir[choice];
                demandFile = dir[choice] + ".txt";
                shellFile = dir[choice] + ".sh";
                recordFile = dir[choice] + ".cast";
                shellDisplay->changeFile(workDir/lab/shellFile);
                shellDisplay->run();
                demandDisplay->changeFile(workDir/lab / demandFile);
                demandDisplay->run();
                top_panel(mainPanel);
                update_panels();
                doupdate();
            }
            break;
        }

        case 'c':
        {
            bool run1 = true;
            top_panel(checkPanel);
            update_panels();
            doupdate();
            runShellCheck(workDir/lab/shellFile);
            checkDisplay->reloadFile();
            while (run1)
            {
                int ch = wgetch(checkWin);
                if (ch == 'q')
                {
                    run1 = false;
                    break;
                }
                else
                {
                    checkDisplay->handleInput(ch);
                    run1 = true;
                }
            }
            top_panel(mainPanel);
            update_panels();
            doupdate();
            break;
        }

        case 'g':
        {
            top_panel(gitPanel);
            update_panels();
            doupdate();
            std::string commitMessage = git->run();
            //commitMessage.erase(std::remove(commitMessage.begin(), commitMessage.end(), ' '), commitMessage.end());
            git->reinitialize(lab);
            git_add_all(workDir);
            use_git_commit(workDir, commitMessage);
            curs_set(0);
            top_panel(mainPanel);
            update_panels();
            doupdate();
            break;
        }

        case 'q':
        {
            top_panel(exitPanel);
            update_panels();
            doupdate();
            int choice = labExit->run();
            if (choice == -1)
            {
                top_panel(mainPanel);
                update_panels();
                doupdate();
            }
            else if (choice == 0)
            {
                use_git_push(workDir, gitUrl);
                run = false;
            }
            else
            {
                run = false;
            }
            break;
        }

        case 's':
        {
            record_with_asciinema(editor, workDir / lab / shellFile, workDir / lab / recordFile);
            shellDisplay->reloadFile();
            break;
        }

        default:
            break;
        }
    }

    // 清理释放
    delete shellDisplay;
    delete demandDisplay;
    delete checkDisplay;
    delete git;
    delete labChoice;
    delete labExit;

    del_panel(exitPanel);
    del_panel(labPanel);
    del_panel(checkPanel);
    del_panel(gitPanel);
    del_panel(mainPanel);

    delwin(exitWin);
    delwin(labWin);
    delwin(checkWin);
    delwin(gitWin);
    delwin(buttonWIN);
    delwin(demandWin);
    delwin(shellWin);
    delwin(mainWin);
}

int main(int argc, char *argv[])
{

    if (argc < 2)
    {
        std::cout << "Usage: " << argv[0] << " <dirname,your git repository>" << std::endl;
        return 1;
    }
    git_libgit2_init();
    // 初始化ncurses
    initscr();
    start_color();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    refresh();
    welcome();

    std::filesystem::path workDir = argv[1];
    if (std::filesystem::exists(workDir))
    {
        if (!std::filesystem::is_directory(workDir))
        {
            std::cout << "Error: " << workDir << " is not a directory." << std::endl;
            return 1;
        }
        if (std::filesystem::is_empty(workDir))
        {
            init(workDir);
        }
        else if (std::filesystem::exists(workDir / ".git"))
        {
            std::string workDirStr = workDir.string();
            //system("git pull ");
            use_git_pull(workDirStr);
        }
        else
        {
            std::cout << "Error: " << workDir << " is not a git repository." << std::endl;
            return 1;
        }
    }
    else
    {
        std::cout << "Error: " << workDir << " does not exist." << std::endl;
        return 1;
    }

    std::ifstream file(workDir / "student.json");
    nlohmann::json student;
    file >> student;
    file.close();
    std::string lab = lab_choice(student);
    refresh();
    mainProgram(student, workDir, lab);
    clear();
    endwin();
    git_libgit2_shutdown();
    return 0;
}