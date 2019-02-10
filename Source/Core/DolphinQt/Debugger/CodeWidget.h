// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <QDockWidget>
#include <QString>

#include "Common/CommonTypes.h"
#include "DolphinQt/Debugger/CodeDiffDialog.h"
#include "DolphinQt/Debugger/CodeTraceDialog.h"
#include "DolphinQt/Debugger/CodeViewWidget.h"

class QCloseEvent;
class QLineEdit;
class QShowEvent;
class QSplitter;
class QListWidget;
class QPushButton;

namespace Common
{
struct Symbol;
}

class CodeWidget : public QDockWidget
{
  Q_OBJECT
public:
  explicit CodeWidget(QWidget* parent = nullptr);
  ~CodeWidget();

  void Step();
  void StepOver();
  void StepOut();
  void Skip();
  void ShowPC();
  void SetPC();

  void OnTrace();
  void OnDiff();
  void ToggleBreakpoint();
  void AddBreakpoint();
  void SetAddress(u32 address, CodeViewWidget::SetAddressUpdate update);

  void Update();
  void UpdateSymbols();

signals:
  void BreakpointsChanged();
  void RequestPPCComparison(u32 addr);
  void ShowMemory(u32 address);

private:
  void CreateWidgets();
  void ConnectWidgets();
  void UpdateCallstack();
  void UpdateFunctionCalls(const Common::Symbol* symbol);
  void UpdateFunctionCallers(const Common::Symbol* symbol);
  void UpdateNotes();

  void OnSearchAddress();
  void OnSearchSymbols();
  void OnSelectSymbol();
  void OnSelectNote();
  void OnSelectCallstack();
  void OnSelectFunctionCallers();
  void OnSelectFunctionCalls();

  void closeEvent(QCloseEvent*) override;
  void showEvent(QShowEvent* event) override;
  bool eventFilter(QObject* obj, QEvent* event);

  CodeTraceDialog* trace_dialog = nullptr;
  CodeDiffDialog* diff_dialog = nullptr;
  QLineEdit* m_search_address;
  QLineEdit* m_search_symbols;
  QPushButton* m_code_trace;
  QPushButton* m_code_diff;
  QListWidget* m_callstack_list;
  QListWidget* m_symbols_list;
  QListWidget* m_note_list;
  QListWidget* m_function_calls_list;
  QListWidget* m_function_callers_list;
  CodeViewWidget* m_code_view;
  QSplitter* m_box_splitter;
  QSplitter* m_code_splitter;

  QString m_symbol_filter;
};
