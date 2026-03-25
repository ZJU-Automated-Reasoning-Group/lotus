/*
 *  Canary features a fast unification-based alias analysis for C programs
 *  Copyright (C) 2021 Qingkai Shi <qingkaishi@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as published
 *  by the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef DYCKAA_DYCKGRAPHEDGELABEL_H
#define DYCKAA_DYCKGRAPHEDGELABEL_H

#include <map>
#include <string>

class DyckGraphEdgeLabel {
public:
  enum LabelType { LT_Dereference, LT_Offset, LT_Index };

private:
  std::string Desc;

public:
  virtual std::string &getEdgeLabelDescription() { return Desc; }

  virtual bool isLabelTy(LabelType type) { return false; }

  virtual ~DyckGraphEdgeLabel() = default;
};

class DereferenceEdgeLabel : public DyckGraphEdgeLabel {
public:
  DereferenceEdgeLabel() {
    std::string &Desc = DyckGraphEdgeLabel::getEdgeLabelDescription();
    Desc.clear();
    Desc.append("D");
  }

  bool isLabelTy(LabelType Ty) override {
    return Ty == DyckGraphEdgeLabel::LT_Dereference;
  }
};

class PointerOffsetEdgeLabel : public DyckGraphEdgeLabel {
private:
  long OffsetBytes;

public:
  explicit PointerOffsetEdgeLabel(long Bytes) : OffsetBytes(Bytes) {
    std::string &Desc = DyckGraphEdgeLabel::getEdgeLabelDescription();
    Desc.clear();
    Desc.append("@");

    char Temp[1024];
    snprintf(Temp, sizeof(Temp), "%ld", Bytes);
    Desc.append(Temp);
  }

  long getOffsetBytes() const { return OffsetBytes; }

  bool isLabelTy(LabelType Ty) override {
    return Ty == DyckGraphEdgeLabel::LT_Offset;
  }
};

class FieldIndexEdgeLabel : public DyckGraphEdgeLabel {
private:
  long FieldIndex;

public:
  explicit FieldIndexEdgeLabel(long Idx) : FieldIndex(Idx) {
    std::string &Desc = DyckGraphEdgeLabel::getEdgeLabelDescription();
    Desc.clear();
    Desc.append("#");

    char Temp[1024];
    snprintf(Temp, sizeof(Temp), "%ld", Idx);
    Desc.append(Temp);
  }

  long getFieldIndex() const { return FieldIndex; }

  bool isLabelTy(LabelType Ty) override {
    return Ty == DyckGraphEdgeLabel::LT_Index;
  }
};

#endif // DYCKAA_DYCKGRAPHEDGELABEL_H
