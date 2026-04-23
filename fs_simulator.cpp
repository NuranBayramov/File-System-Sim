#include <bits/stdc++.h>

using namespace std;

enum AllocType {
    CONTIGUOUS,
    FAT_ALLOC,
    INODE_ALLOC
};

struct JournalEntry {
    string text;
    bool committed;
};

struct INode {
    int id;
    bool isDirectory;
    bool isSymlink;
    int sizeBlocks;
    int refCount;
    int openCount;
    string symlinkTarget;

    // for all allocation methods we keep the "real" list of blocks here too,
    // it makes read / delete / stats easier in this simulator
    vector<int> blocks;

    // contiguous
    int startBlock;

    // i-node style
    vector<int> directBlocks;
    vector<int> indirectBlocks;

    INode() {
        id = -1;
        isDirectory = false;
        isSymlink = false;
        sizeBlocks = 0;
        refCount = 1;
        openCount = 0;
        startBlock = -1;
    }
};

struct DirEntry {
    string name;
    int inodeId;
};

struct Stats {
    int reads = 0;
    int writes = 0;
    int creates = 0;
    int deletes = 0;
    int opens = 0;
    int closes = 0;
};

class FileSystemSimulator {
private:
    int diskBlocks;
    int blockSize;
    AllocType allocType;
    int nextInodeId;
    int directLimit;

    vector<bool> freeBlocks;
    vector<int> fatTable; // -2 free, -1 end, else next block
    map<int, shared_ptr<INode>> inodeTable;
    map<int, map<string, int>> directories; // inodeId -> name -> inodeId
    vector<JournalEntry> journal;
    Stats stats;

    int rootId;
    int memoryLoadedInodes;

public:
    FileSystemSimulator(int dBlocks, int bSize, AllocType type) {
        diskBlocks = dBlocks;
        blockSize = bSize;
        allocType = type;
        nextInodeId = 1;
        directLimit = 4;
        freeBlocks.assign(diskBlocks, true);
        fatTable.assign(diskBlocks, -2);
        memoryLoadedInodes = 0;

        auto root = make_shared<INode>();
        root->id = nextInodeId++;
        root->isDirectory = true;
        root->refCount = 1;
        inodeTable[root->id] = root;
        directories[root->id] = map<string, int>();
        rootId = root->id;
    }

    void runWorkload(const string& fileName) {
        ifstream fin(fileName);
        if (!fin.is_open()) {
            cout << "Could not open workload file.\n";
            return;
        }

        string line;
        while (getline(fin, line)) {
            if (line.empty()) continue;
            if (line[0] == '#') continue;
            executeLine(line);
        }
        fin.close();
    }

    void executeLine(const string& line) {
        stringstream ss(line);
        string cmd;
        ss >> cmd;

        if (cmd == "CREATE") {
            string path;
            int blocks;
            ss >> path >> blocks;
            createFile(path, blocks);
        } else if (cmd == "CREATE_DIR") {
            string path;
            ss >> path;
            createDirectory(path);
        } else if (cmd == "DELETE") {
            string path;
            ss >> path;
            deletePath(path);
        } else if (cmd == "OPEN") {
            string path;
            ss >> path;
            openFile(path);
        } else if (cmd == "CLOSE") {
            string path;
            ss >> path;
            closeFile(path);
        } else if (cmd == "READ") {
            string path;
            ss >> path;
            readFile(path);
        } else if (cmd == "WRITE") {
            string path;
            int extra;
            ss >> path >> extra;
            writeFile(path, extra);
        } else if (cmd == "HARDLINK") {
            string existingPath, newPath;
            ss >> existingPath >> newPath;
            createHardLink(existingPath, newPath);
        } else if (cmd == "SOFTLINK") {
            string targetPath, newPath;
            ss >> targetPath >> newPath;
            createSoftLink(targetPath, newPath);
        } else if (cmd == "LIST") {
            string path;
            ss >> path;
            listDirectory(path);
        } else if (cmd == "CRASH") {
            simulateCrashRecovery();
        } else if (cmd == "STATUS") {
            printStatus();
        } else {
            cout << "Unknown command: " << line << "\n";
        }
    }

    void createDirectory(const string& path) {
        int parentId;
        string name;
        if (!splitParent(path, parentId, name)) {
            cout << "CREATE_DIR failed: invalid parent path " << path << "\n";
            return;
        }
        if (directories[parentId].count(name)) {
            cout << "CREATE_DIR failed: already exists " << path << "\n";
            return;
        }

        auto node = make_shared<INode>();
        node->id = nextInodeId++;
        node->isDirectory = true;
        inodeTable[node->id] = node;
        directories[node->id] = map<string, int>();
        directories[parentId][name] = node->id;

        cout << "Directory created: " << path << "\n";
    }

    void createFile(const string& path, int blocksNeeded) {
        int parentId;
        string name;
        if (!splitParent(path, parentId, name)) {
            cout << "CREATE failed: invalid parent path " << path << "\n";
            return;
        }
        if (directories[parentId].count(name)) {
            cout << "CREATE failed: already exists " << path << "\n";
            return;
        }

        auto node = make_shared<INode>();
        node->id = nextInodeId++;
        node->isDirectory = false;
        node->sizeBlocks = blocksNeeded;

        bool ok = allocateBlocks(node, blocksNeeded);
        if (!ok) {
            cout << "CREATE failed: not enough space for " << path << "\n";
            return;
        }

        inodeTable[node->id] = node;
        directories[parentId][name] = node->id;
        stats.creates++;

        cout << "File created: " << path << " (" << blocksNeeded << " blocks)\n";
    }

    void deletePath(const string& path) {
        if (path == "/") {
            cout << "DELETE failed: cannot delete root\n";
            return;
        }

        int parentId;
        string name;
        if (!splitParent(path, parentId, name)) {
            cout << "DELETE failed: bad path " << path << "\n";
            return;
        }
        if (!directories[parentId].count(name)) {
            cout << "DELETE failed: not found " << path << "\n";
            return;
        }

        int inodeId = directories[parentId][name];
        auto node = inodeTable[inodeId];

        if (node->isDirectory && !directories[inodeId].empty()) {
            cout << "DELETE failed: directory not empty " << path << "\n";
            return;
        }

        addJournal("DELETE start " + path);
        addJournal("remove directory entry " + path);

        directories[parentId].erase(name);

        if (node->isSymlink) {
            addJournal("release symlink inode " + path);
            inodeTable.erase(inodeId);
            addJournal("DELETE finished " + path);
            commitLastJournalEntries(4);
            cout << "Deleted symlink: " << path << "\n";
            stats.deletes++;
            return;
        }

        node->refCount--;

        if (node->refCount <= 0) {
            addJournal("release inode " + path);
            addJournal("return disk blocks " + path);
            freeNodeBlocks(node);
            if (node->isDirectory) {
                directories.erase(inodeId);
            }
            inodeTable.erase(inodeId);
        }

        addJournal("DELETE finished " + path);
        commitLastJournalEntries(5);

        cout << "Deleted: " << path << "\n";
        stats.deletes++;
    }

    void openFile(const string& path) {
        auto node = resolvePath(path, true);
        if (!node) {
            cout << "OPEN failed: not found or broken link " << path << "\n";
            return;
        }
        if (node->isDirectory) {
            cout << "OPEN failed: path is a directory " << path << "\n";
            return;
        }
        node->openCount++;
        if (allocType == INODE_ALLOC && node->openCount == 1) {
            memoryLoadedInodes++;
        }
        stats.opens++;
        cout << "Opened: " << path << "\n";
    }

    void closeFile(const string& path) {
        auto node = resolvePath(path, true);
        if (!node) {
            cout << "CLOSE failed: not found " << path << "\n";
            return;
        }
        if (node->openCount > 0) {
            node->openCount--;
            if (allocType == INODE_ALLOC && node->openCount == 0) {
                memoryLoadedInodes--;
            }
            stats.closes++;
            cout << "Closed: " << path << "\n";
        } else {
            cout << "CLOSE failed: file not open " << path << "\n";
        }
    }

    void readFile(const string& path) {
        auto node = resolvePath(path, true);
        if (!node) {
            cout << "READ failed: not found or broken link " << path << "\n";
            return;
        }
        if (node->isDirectory) {
            cout << "READ failed: directory " << path << "\n";
            return;
        }

        stats.reads++;
        cout << "Read " << path << ": ";
        if (allocType == CONTIGUOUS) {
            cout << "Contiguous allocation => one seek to first block, then sequential blocks. ";
        } else if (allocType == FAT_ALLOC) {
            cout << "FAT allocation => follow pointers using in-memory FAT. ";
        } else {
            cout << "I-node allocation => consult direct/indirect pointers from inode. ";
        }

        cout << "Blocks: ";
        for (int b : node->blocks) cout << b << " ";
        cout << "\n";
    }

    void writeFile(const string& path, int extraBlocks) {
        auto node = resolvePath(path, true);
        if (!node) {
            cout << "WRITE failed: not found or broken link " << path << "\n";
            return;
        }
        if (node->isDirectory) {
            cout << "WRITE failed: directory " << path << "\n";
            return;
        }

        bool ok = extendFile(node, extraBlocks);
        if (!ok) {
            cout << "WRITE failed: could not grow file " << path << "\n";
            return;
        }

        stats.writes++;
        cout << "Write complete: " << path << " grew by " << extraBlocks << " blocks\n";
    }

    void createHardLink(const string& existingPath, const string& newPath) {
        auto node = resolvePath(existingPath, false);
        if (!node) {
            cout << "HARDLINK failed: source not found " << existingPath << "\n";
            return;
        }
        if (node->isDirectory) {
            cout << "HARDLINK failed: directories not supported for hard links here\n";
            return;
        }

        int parentId;
        string name;
        if (!splitParent(newPath, parentId, name)) {
            cout << "HARDLINK failed: bad new path\n";
            return;
        }
        if (directories[parentId].count(name)) {
            cout << "HARDLINK failed: destination exists\n";
            return;
        }

        directories[parentId][name] = node->id;
        node->refCount++;
        cout << "Hard link created: " << newPath << " -> inode " << node->id << "\n";
    }

    void createSoftLink(const string& targetPath, const string& newPath) {
        int parentId;
        string name;
        if (!splitParent(newPath, parentId, name)) {
            cout << "SOFTLINK failed: bad new path\n";
            return;
        }
        if (directories[parentId].count(name)) {
            cout << "SOFTLINK failed: destination exists\n";
            return;
        }

        auto node = make_shared<INode>();
        node->id = nextInodeId++;
        node->isSymlink = true;
        node->symlinkTarget = targetPath;
        inodeTable[node->id] = node;
        directories[parentId][name] = node->id;

        cout << "Soft link created: " << newPath << " -> " << targetPath << "\n";
    }

    void listDirectory(const string& path) {
        auto node = resolvePath(path, false);
        if (!node || !node->isDirectory) {
            cout << "LIST failed: not a directory " << path << "\n";
            return;
        }

        cout << "Directory " << path << ":\n";
        for (auto& p : directories[node->id]) {
            auto child = inodeTable[p.second];
            cout << "  " << p.first;
            if (child->isDirectory) cout << " [DIR]";
            else if (child->isSymlink) cout << " [SOFTLINK -> " << child->symlinkTarget << "]";
            else cout << " [FILE, refs=" << child->refCount << "]";
            cout << "\n";
        }
    }

    void printStatus() {
        cout << "\n===== FILE SYSTEM STATUS =====\n";
        cout << "Allocation method: ";
        if (allocType == CONTIGUOUS) cout << "Contiguous\n";
        else if (allocType == FAT_ALLOC) cout << "FAT\n";
        else cout << "I-node\n";

        cout << "Total blocks: " << diskBlocks << "\n";
        cout << "Free blocks: " << countFreeBlocks() << "\n";
        cout << "Used blocks: " << diskBlocks - countFreeBlocks() << "\n";
        cout << "External fragmentation (contiguous only): " << externalFragmentation() << "\n";
        cout << "FAT memory overhead (entries in memory): " << fatMemoryOverhead() << "\n";
        cout << "Loaded i-nodes in memory (open files only): " << memoryLoadedInodes << "\n";

        cout << "Journal entries: " << journal.size() << "\n";
        cout << "Ops => create:" << stats.creates
             << " delete:" << stats.deletes
             << " open:" << stats.opens
             << " close:" << stats.closes
             << " read:" << stats.reads
             << " write:" << stats.writes << "\n";

        cout << "==============================\n\n";
    }

    void simulateCrashRecovery() {
        cout << "Simulating crash and journal recovery...\n";
        for (auto& j : journal) {
            if (!j.committed) {
                cout << "Uncommitted journal record found: " << j.text << "\n";
                cout << "Recovery action: operation would be rolled back or replayed in a real FS.\n";
            }
        }
        cout << "Recovery scan complete.\n";
    }

private:
    vector<string> splitPath(const string& path) {
        vector<string> parts;
        string part;
        stringstream ss(path);
        while (getline(ss, part, '/')) {
            if (!part.empty()) parts.push_back(part);
        }
        return parts;
    }

    bool splitParent(const string& path, int& parentId, string& name) {
        vector<string> parts = splitPath(path);
        if (parts.empty()) return false;

        name = parts.back();
        parts.pop_back();

        parentId = rootId;
        for (string p : parts) {
            if (!directories[parentId].count(p)) return false;
            int nextId = directories[parentId][p];
            if (!inodeTable.count(nextId)) return false;
            if (!inodeTable[nextId]->isDirectory) return false;
            parentId = nextId;
        }
        return true;
    }

    shared_ptr<INode> resolvePath(const string& path, bool followSymlink) {
        if (path == "/") return inodeTable[rootId];

        vector<string> parts = splitPath(path);
        int cur = rootId;

        for (int i = 0; i < (int)parts.size(); i++) {
            if (!directories[cur].count(parts[i])) return nullptr;
            int nextId = directories[cur][parts[i]];
            if (!inodeTable.count(nextId)) return nullptr;
            auto node = inodeTable[nextId];

            if (node->isSymlink && followSymlink) {
                auto target = resolvePath(node->symlinkTarget, false);
                if (!target) return nullptr; // broken soft link
                node = target;
                nextId = node->id;
            }

            if (i != (int)parts.size() - 1) {
                if (!node->isDirectory) return nullptr;
                cur = nextId;
            } else {
                return node;
            }
        }
        return nullptr;
    }

    bool allocateBlocks(shared_ptr<INode> node, int blocksNeeded) {
        if (blocksNeeded == 0) return true;

        if (allocType == CONTIGUOUS) {
            int start = findContiguousRun(blocksNeeded);
            if (start == -1) return false;

            node->startBlock = start;
            node->blocks.clear();
            for (int i = 0; i < blocksNeeded; i++) {
                freeBlocks[start + i] = false;
                node->blocks.push_back(start + i);
            }
            node->sizeBlocks = blocksNeeded;
            return true;
        }

        vector<int> chosen;
        for (int i = 0; i < diskBlocks && (int)chosen.size() < blocksNeeded; i++) {
            if (freeBlocks[i]) chosen.push_back(i);
        }
        if ((int)chosen.size() < blocksNeeded) return false;

        for (int b : chosen) freeBlocks[b] = false;
        node->blocks = chosen;
        node->sizeBlocks = blocksNeeded;

        if (allocType == FAT_ALLOC) {
            for (int i = 0; i < (int)chosen.size(); i++) {
                if (i == (int)chosen.size() - 1) fatTable[chosen[i]] = -1;
                else fatTable[chosen[i]] = chosen[i + 1];
            }
        } else if (allocType == INODE_ALLOC) {
            node->directBlocks.clear();
            node->indirectBlocks.clear();
            for (int i = 0; i < (int)chosen.size(); i++) {
                if (i < directLimit) node->directBlocks.push_back(chosen[i]);
                else node->indirectBlocks.push_back(chosen[i]);
            }
        }

        return true;
    }

    bool extendFile(shared_ptr<INode> node, int extraBlocks) {
        if (extraBlocks <= 0) return true;

        if (allocType == CONTIGUOUS) {
            if (node->blocks.empty()) {
                return allocateBlocks(node, extraBlocks);
            }

            int endBlock = node->blocks.back();
            bool canExtend = true;
            for (int i = 1; i <= extraBlocks; i++) {
                if (endBlock + i >= diskBlocks || !freeBlocks[endBlock + i]) {
                    canExtend = false;
                    break;
                }
            }

            if (canExtend) {
                for (int i = 1; i <= extraBlocks; i++) {
                    freeBlocks[endBlock + i] = false;
                    node->blocks.push_back(endBlock + i);
                }
                node->sizeBlocks += extraBlocks;
                return true;
            } else {
                // simple student-style relocation: try to move whole file elsewhere
                int newSize = node->sizeBlocks + extraBlocks;
                int newStart = findContiguousRun(newSize);
                if (newStart == -1) return false;

                for (int b : node->blocks) freeBlocks[b] = true;
                node->blocks.clear();
                node->startBlock = newStart;
                for (int i = 0; i < newSize; i++) {
                    freeBlocks[newStart + i] = false;
                    node->blocks.push_back(newStart + i);
                }
                node->sizeBlocks = newSize;
                return true;
            }
        }

        vector<int> extra;
        for (int i = 0; i < diskBlocks && (int)extra.size() < extraBlocks; i++) {
            if (freeBlocks[i]) extra.push_back(i);
        }
        if ((int)extra.size() < extraBlocks) return false;

        for (int b : extra) freeBlocks[b] = false;

        if (allocType == FAT_ALLOC) {
            if (!node->blocks.empty()) {
                fatTable[node->blocks.back()] = extra[0];
            }
            for (int i = 0; i < (int)extra.size(); i++) {
                if (i == (int)extra.size() - 1) fatTable[extra[i]] = -1;
                else fatTable[extra[i]] = extra[i + 1];
            }
        } else if (allocType == INODE_ALLOC) {
            for (int b : extra) {
                if ((int)node->directBlocks.size() < directLimit) node->directBlocks.push_back(b);
                else node->indirectBlocks.push_back(b);
            }
        }

        for (int b : extra) node->blocks.push_back(b);
        node->sizeBlocks += extraBlocks;
        return true;
    }

    void freeNodeBlocks(shared_ptr<INode> node) {
        if (allocType == CONTIGUOUS) {
            for (int b : node->blocks) {
                freeBlocks[b] = true;
            }
        } else if (allocType == FAT_ALLOC) {
            for (int b : node->blocks) {
                freeBlocks[b] = true;
                fatTable[b] = -2;
            }
        } else {
            for (int b : node->blocks) {
                freeBlocks[b] = true;
            }
            node->directBlocks.clear();
            node->indirectBlocks.clear();
        }

        node->blocks.clear();
        node->sizeBlocks = 0;
        node->startBlock = -1;
    }

    int findContiguousRun(int need) {
        int count = 0;
        int start = -1;
        for (int i = 0; i < diskBlocks; i++) {
            if (freeBlocks[i]) {
                if (count == 0) start = i;
                count++;
                if (count == need) return start;
            } else {
                count = 0;
                start = -1;
            }
        }
        return -1;
    }

    int countFreeBlocks() {
        int c = 0;
        for (bool x : freeBlocks) if (x) c++;
        return c;
    }

    int externalFragmentation() {
        int holes = 0;
        bool inHole = false;
        for (int i = 0; i < diskBlocks; i++) {
            if (freeBlocks[i] && !inHole) {
                holes++;
                inHole = true;
            } else if (!freeBlocks[i]) {
                inHole = false;
            }
        }
        return holes;
    }

    int fatMemoryOverhead() {
        if (allocType != FAT_ALLOC) return 0;
        // just number of entries in table as a simple simulation metric
        return (int)fatTable.size();
    }

    void addJournal(const string& text) {
        JournalEntry j;
        j.text = text;
        j.committed = false;
        journal.push_back(j);
    }

    void commitLastJournalEntries(int count) {
        for (int i = (int)journal.size() - 1; i >= 0 && count > 0; i--, count--) {
            journal[i].committed = true;
        }
    }
};

AllocType parseAllocType(const string& s) {
    if (s == "contiguous") return CONTIGUOUS;
    if (s == "fat") return FAT_ALLOC;
    return INODE_ALLOC;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        cout << "Usage: ./simulator <contiguous|fat|inode> <workload.txt>\n";
        return 0;
    }

    string typeStr = argv[1];
    string workload = argv[2];

    AllocType type = parseAllocType(typeStr);

    // small disk so fragmentation is easy to see in testing
    FileSystemSimulator fs(64, 512, type);
    fs.runWorkload(workload);
    fs.printStatus();

    return 0;
}