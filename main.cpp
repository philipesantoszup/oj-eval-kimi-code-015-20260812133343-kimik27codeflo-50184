#include <bits/stdc++.h>
#include <unistd.h>
using namespace std;

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    int n;
    if (!(cin >> n)) return 0;
    string cmd;
    map<string, set<int>> db;
    for (int i = 0; i < n; ++i) {
        cin >> cmd;
        if (cmd == "insert") {
            string idx;
            int val;
            cin >> idx >> val;
            db[idx].insert(val);
        } else if (cmd == "delete") {
            string idx;
            int val;
            cin >> idx >> val;
            auto it = db.find(idx);
            if (it != db.end()) {
                it->second.erase(val);
                if (it->second.empty()) db.erase(it);
            }
        } else if (cmd == "find") {
            string idx;
            cin >> idx;
            auto it = db.find(idx);
            if (it == db.end() || it->second.empty()) {
                cout << "null\n";
            } else {
                bool first = true;
                for (int v : it->second) {
                    if (!first) cout << ' ';
                    first = false;
                    cout << v;
                }
                cout << '\n';
            }
        }
    }
    return 0;
}
