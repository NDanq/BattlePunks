#ifndef NODEGRAPH_CODEWRITER_H
#define NODEGRAPH_CODEWRITER_H

#include <string>
#include <vector>

#include "Public/NodeGraph/Graph.h"

/*
 * Line-at-a-time source output: four-space indentation, '\n' endings, and for
 * each line the node that produced it (CompileResult::LineNodes).
 */
namespace NodeGraph
{
    class CodeWriter
    {
    public:
        void Line(const std::string& InText, const NodeGuid& InNode = NodeGuid())
        {
            if(!InText.empty()) { Text.append(size_t(Depth) * 4, ' '); }
            Text += InText;
            Text += '\n';
            LineNodes.push_back(InNode);
        }

        void Blank() { Line(std::string()); }

        void Indent()  { ++Depth; }
        void Outdent() { if(Depth > 0) { --Depth; } }

        /* Appends another writer's lines at this one's depth. */
        void Append(const CodeWriter& InOther)
        {
            size_t Start = 0;
            size_t LineIndex = 0;
            while(Start < InOther.Text.size())
            {
                const size_t End = InOther.Text.find('\n', Start);
                const std::string Piece = InOther.Text.substr(Start, End - Start);
                Line(Piece, LineIndex < InOther.LineNodes.size() ? InOther.LineNodes[LineIndex] : NodeGuid());
                ++LineIndex;
                if(End == std::string::npos) { break; }
                Start = End + 1;
            }
        }

        bool IsEmpty() const { return Text.empty(); }

        std::string           Text;
        std::vector<NodeGuid> LineNodes;

    private:
        int Depth = 0;
    };
}

#endif //NODEGRAPH_CODEWRITER_H
