// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "DolphinQt/CheatsManager.h"

#include <algorithm>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include "Common/BitUtils.h"
#include "Core/ActionReplay.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"

#include "UICommon/GameFile.h"

#include "DolphinQt/Config/ARCodeWidget.h"
#include "DolphinQt/Config/GeckoCodeWidget.h"
#include "DolphinQt/GameList/GameListModel.h"
#include "DolphinQt/Settings.h"

constexpr int MAX_RESULTS = 4096;

constexpr int INDEX_ROLE = Qt::UserRole;
constexpr int COLUMN_ROLE = Qt::UserRole + 1;

constexpr int AR_SET_BYTE_CMD = 0x00;
constexpr int AR_SET_SHORT_CMD = 0x02;
constexpr int AR_SET_INT_CMD = 0x04;

enum class CompareType : int
{
  Equal = 0,
  NotEqual = 1,
  Less = 2,
  LessEqual = 3,
  More = 4,
  MoreEqual = 5
};

enum class DataType : int
{
  Byte = 0,
  Short = 1,
  Int = 2,
  Float = 3,
  Double = 4,
  String = 5
};

CheatsManager::CheatsManager(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("Cheats Manager"));
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &CheatsManager::OnStateChanged);

  OnStateChanged(Core::GetState());

  CreateWidgets();
  ConnectWidgets();
  Reset();
  Update();
}

CheatsManager::~CheatsManager() = default;

void CheatsManager::reject()
{
  m_timer->stop();
  QDialog::reject();
}

void CheatsManager::OnStateChanged(Core::State state)
{
  if (state != Core::State::Running && state != Core::State::Paused)
    return;

  auto* model = Settings::Instance().GetGameListModel();

  for (int i = 0; i < model->rowCount(QModelIndex()); i++)
  {
    auto file = model->GetGameFile(i);

    if (file->GetGameID() == SConfig::GetInstance().GetGameID())
    {
      m_game_file = file;
      if (m_tab_widget->count() == 3)
      {
        m_tab_widget->removeTab(0);
        m_tab_widget->removeTab(0);
      }

      if (m_tab_widget->count() == 1)
      {
        if (m_ar_code)
          m_ar_code->deleteLater();

        m_ar_code = new ARCodeWidget(*m_game_file, false);
        m_tab_widget->insertTab(0, m_ar_code, tr("AR Code"));
        m_tab_widget->insertTab(1, new GeckoCodeWidget(*m_game_file, false), tr("Gecko Codes"));
      }
    }
  }
}

void CheatsManager::CreateWidgets()
{
  m_tab_widget = new QTabWidget;
  m_button_box = new QDialogButtonBox(QDialogButtonBox::Close);

  m_cheat_search = CreateCheatSearch();

  m_tab_widget->addTab(m_cheat_search, tr("Cheat Search"));

  auto* layout = new QVBoxLayout;
  layout->addWidget(m_tab_widget);
  layout->addWidget(m_button_box);

  setLayout(layout);
}

void CheatsManager::ConnectWidgets()
{
  connect(m_button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_timer, &QTimer::timeout, this, &CheatsManager::TimedUpdate);
  connect(m_match_new, &QPushButton::clicked, this, &CheatsManager::OnNewSearchClicked);
  connect(m_match_next, &QPushButton::clicked, this, &CheatsManager::NextSearch);
  connect(m_match_refresh, &QPushButton::clicked, this, &CheatsManager::Update);
  connect(m_match_reset, &QPushButton::clicked, this, &CheatsManager::Reset);
  for (auto* radio : {m_ram_main, m_ram_wii, m_ram_fakevmem})
    connect(radio, &QRadioButton::toggled, this, &CheatsManager::MemoryPtr);

  m_match_table->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(m_match_table, &QTableWidget::customContextMenuRequested, this,
          &CheatsManager::OnMatchContextMenu);
  connect(m_refresh, qOverload<int>(&QSpinBox::valueChanged),
          [this](int interval) { m_timer->setInterval(interval); });
  connect(m_refresh_enabled, &QCheckBox::stateChanged, [this](bool enabled) {
    if (enabled)
    {
      m_timer->setSingleShot(false);
      m_timer->start();
    }
    else
    {
      m_timer->setSingleShot(true);
    }
  });
}

QWidget* CheatsManager::CreateCheatSearch()
{
  m_match_table = new QTableWidget;

  m_match_table->setTabKeyNavigation(false);
  m_match_table->setColumnCount(4);
  m_match_table->verticalHeader()->hide();
  m_match_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_match_table->setHorizontalHeaderLabels(
      {tr("Address"), tr("Hexadecimal"), tr("Decimal"), tr("Float")});
  m_match_table->setColumnWidth(3, m_match_table->columnWidth(3) * 1.5);
  m_match_table->setFixedWidth(m_match_table->horizontalHeader()->length() + 2);

  m_match_table->setSelectionBehavior(QAbstractItemView::SelectRows);

  // Fixed width appears to look best. This adds an empty spaceritem to the left of the table,
  // so it can absorb extra space.
  auto* table_with_space = new QWidget;
  auto* space_layout = new QHBoxLayout;
  table_with_space->setLayout(space_layout);
  space_layout->addStretch();
  space_layout->addWidget(m_match_table);

  // Options
  m_result_label = new QLabel;
  m_match_length = new QComboBox;
  m_match_operation = new QComboBox;
  m_match_value = new QLineEdit;
  m_match_new = new QPushButton(tr("Initialize"));
  m_match_next = new QPushButton(tr("Next Search"));
  m_match_refresh = new QPushButton(tr("Refresh"));
  m_match_reset = new QPushButton(tr("Reset"));

  auto* options = new QWidget;
  auto* layout = new QVBoxLayout;
  options->setLayout(layout);

  for (const auto& option : {tr("8-bit"), tr("16-bit"), tr("32-bit"), tr("Float")})
  {
    m_match_length->addItem(option);
  }

  m_match_length->setCurrentIndex(2);

  for (const auto& option :
       {tr("Unknown"), tr("Not Equal"), tr("Equal"), tr("Greater than"), tr("Less than")})
  {
    m_match_operation->addItem(option);
  }

  auto* group_box = new QGroupBox(tr("Type"));
  auto* group_layout = new QHBoxLayout;
  group_box->setLayout(group_layout);

  // i18n: The base 10 numeral system. Not related to non-integer numbers
  m_match_decimal = new QRadioButton(tr("Decimal"));
  m_match_hexadecimal = new QRadioButton(tr("Hexadecimal"));
  m_match_octal = new QRadioButton(tr("Octal"));

  group_layout->addWidget(m_match_decimal);
  group_layout->addWidget(m_match_hexadecimal);
  group_layout->addWidget(m_match_octal);
  group_layout->setSpacing(1);

  auto* ram_box = new QGroupBox(tr("Type"));
  auto* ram_layout = new QHBoxLayout;
  ram_box->setLayout(ram_layout);

  m_ram_main = new QRadioButton(tr("Main"));
  m_ram_wii = new QRadioButton(tr("Wii"));
  m_ram_fakevmem = new QRadioButton(tr("FakeVMEM"));

  m_ram_main->setChecked(true);

  ram_layout->addWidget(m_ram_main);
  ram_layout->addWidget(m_ram_wii);
  ram_layout->addWidget(m_ram_fakevmem);
  ram_layout->setSpacing(1);

  auto* range_layout = new QHBoxLayout;
  m_range_start = new QLineEdit(tr("80000000"));
  m_range_end = new QLineEdit(tr("81800000"));
  m_range_start->setMaxLength(8);
  m_range_end->setMaxLength(8);
  range_layout->addWidget(m_range_start);
  range_layout->addWidget(m_range_end);

  auto* refresh_layout = new QHBoxLayout;
  m_refresh_label = new QLabel(tr("Refresh displayed values every"));
  m_refresh = new QSpinBox();
  m_refresh_enabled = new QCheckBox();

  m_refresh->setMinimum(100);
  m_refresh->setMaximum(5000);
  m_refresh->setSingleStep(100);
  m_refresh->setValue(1000);
  m_refresh->setSuffix(tr(" ms"));

  refresh_layout->addWidget(m_refresh_label);
  refresh_layout->addWidget(m_refresh);
  refresh_layout->addWidget(m_refresh_enabled);

  layout->addWidget(m_result_label);
  layout->addWidget(m_match_length);
  layout->addWidget(m_match_operation);
  layout->addWidget(m_match_value);
  layout->addWidget(group_box);
  layout->addWidget(ram_box);
  layout->addLayout(range_layout);
  layout->addWidget(m_match_new);
  layout->addWidget(m_match_next);
  layout->addWidget(m_match_refresh);
  layout->addWidget(m_match_reset);
  layout->addLayout(refresh_layout);

  m_timer = new QTimer();
  m_timer->setInterval(1000);

  // Splitters
  m_option_splitter = new QSplitter(Qt::Horizontal);
  m_table_splitter = new QSplitter(Qt::Vertical);

  m_table_splitter->addWidget(table_with_space);

  m_option_splitter->addWidget(m_table_splitter);
  m_option_splitter->addWidget(options);

  // Only the spacer to the left of the table will expand. There shouldn't be a reason for anything
  // else to.
  m_option_splitter->setStretchFactor(0, 1);
  m_option_splitter->setStretchFactor(1, 0);

  return m_option_splitter;
}

void CheatsManager::MemoryPtr(bool update)
{
  if (m_ram_main->isChecked() && Memory::m_pRAM)
  {
    m_ram.ptr = Memory::m_pRAM;
    m_ram.size = Memory::GetRamSizeReal();
    m_ram.base = 0x80000000;
  }
  else if (m_ram_wii->isChecked() && Memory::m_pEXRAM)
  {
    m_ram.ptr = Memory::m_pEXRAM;
    m_ram.size = Memory::GetExRamSizeReal();
    m_ram.base = 0x90000000;
  }
  else if (m_ram_fakevmem->isChecked() && Memory::m_pFakeVMEM)
  {
    m_ram.ptr = Memory::m_pFakeVMEM;
    m_ram.size = Memory::GetFakeVMemSize();
    m_ram.base = 0x7E000000;
  }
  else
  {
    m_result_label->setText(tr("Memory region is invalid."));
  }

  if (!update)
    return;
  m_range_start->setText(QStringLiteral("%1").arg(m_ram.base, 8, 16));
  m_range_end->setText(QStringLiteral("%1").arg(m_ram.base + m_ram.size, 8, 16));
}

int CheatsManager::GetTypeSize() const
{
  switch (static_cast<DataType>(m_match_length->currentIndex()))
  {
  case DataType::Byte:
    return 1;
  case DataType::Short:
    return 2;
  case DataType::Int:
    return 4;
  case DataType::Float:
    return 4;
  default:
    return 4;
    // return m_match_value->text().toStdString().size();
  }
}

enum class ComparisonMask
{
  EQUAL = 0x1,
  GREATER_THAN = 0x2,
  LESS_THAN = 0x4
};

static ComparisonMask operator|(ComparisonMask comp1, ComparisonMask comp2)
{
  return static_cast<ComparisonMask>(static_cast<int>(comp1) | static_cast<int>(comp2));
}

static ComparisonMask operator&(ComparisonMask comp1, ComparisonMask comp2)
{
  return static_cast<ComparisonMask>(static_cast<int>(comp1) & static_cast<int>(comp2));
}

void CheatsManager::FilterCheatSearchResults(u32 value, bool prev)
{
  static const std::array<ComparisonMask, 5> filters{
      {ComparisonMask::EQUAL | ComparisonMask::GREATER_THAN | ComparisonMask::LESS_THAN,  // Unknown
       ComparisonMask::GREATER_THAN | ComparisonMask::LESS_THAN,  // Not Equal
       ComparisonMask::EQUAL, ComparisonMask::GREATER_THAN, ComparisonMask::LESS_THAN}};
  ComparisonMask filter_mask = filters[m_match_operation->currentIndex()];

  std::vector<Result> filtered_results;
  filtered_results.reserve(m_results.size());

  Core::RunAsCPUThread([&] {
    for (Result& result : m_results)
    {
      if (prev)
        value = result.old_value;

      // with big endian, can just use memcmp for ><= comparison
      int cmp_result = std::memcmp(&m_ram.ptr[result.address], &value, m_search_type_size);
      ComparisonMask cmp_mask;
      if (cmp_result < 0)
        cmp_mask = ComparisonMask::LESS_THAN;
      else if (cmp_result)
        cmp_mask = ComparisonMask::GREATER_THAN;
      else
        cmp_mask = ComparisonMask::EQUAL;

      if (static_cast<int>(cmp_mask & filter_mask))
      {
        std::memcpy(&result.old_value, &m_ram.ptr[result.address], m_search_type_size);
        filtered_results.push_back(result);
      }
    }
  });
  m_results.swap(filtered_results);
}

void CheatsManager::OnNewSearchClicked()
{
  if (!Core::IsRunningAndStarted())
  {
    m_result_label->setText(tr("Game is not currently running."));
    return;
  }

  MemoryPtr(false);

  if (!m_ram.ptr)
    return;

  for (auto* widget : {m_ram_main, m_ram_wii, m_ram_fakevmem})
    widget->setDisabled(true);
  m_range_start->setDisabled(true);
  m_range_end->setDisabled(true);
  m_match_new->setDisabled(true);

  // Determine the user-selected data size for this search.
  m_search_type_size = GetTypeSize();

  // Set up the search results efficiently to prevent automatic re-allocations.
  m_results.clear();
  m_results.reserve(m_ram.size / m_search_type_size);

  // Enable the "Next Scan" button.
  m_scan_is_initialized = true;
  m_match_next->setEnabled(true);
  bool good;

  u32 range_start = 0;
  u32 range_end = static_cast<u32>(m_ram.size);

  u32 custom_start = (m_range_start->text().toUInt(&good, 16) - m_ram.base) & 0xfffffff0;
  if (!good)
    custom_start = range_start;

  u32 custom_end = (m_range_end->text().toUInt(&good, 16) - m_ram.base) & 0xfffffff0;
  if (!good)
    custom_end = range_end;

  if (custom_start > range_start && custom_start < custom_end && custom_start < range_end)
    range_start = custom_start;
  if (custom_end < range_end && custom_end > custom_start && custom_end > range_start)
    range_end = custom_end;

  Core::RunAsCPUThread([&] {
    Result r;
    for (u32 addr = range_start; addr != range_end; addr += m_search_type_size)
    {
      r.address = addr;
      memcpy(&r.old_value, &m_ram.ptr[addr], m_search_type_size);
      m_results.push_back(r);
    }
  });

  Update();
}

void CheatsManager::NextSearch()
{
  if (!m_ram.ptr)
  {
    m_result_label->setText(tr("Memory Not Ready"));
    return;
  }

  int base = 16;
  bool is_float = m_match_length->currentIndex() == 3;

  if (is_float)
  {
    base = 16;
  }
  else
  {
    base = (m_match_decimal->isChecked() ? 10 : (m_match_hexadecimal->isChecked() ? 16 : 8));
  }

  u32 val = 0;
  bool blank_user_value = m_match_value->text().isEmpty();
  if (!blank_user_value)
  {
    bool good;

    if (is_float)
    {
      float value = m_match_value->text().toFloat(&good);

      if (!good)
      {
        m_result_label->setText(tr("Incorrect search value."));
        return;
      }

      val = Common::BitCast<u32>(value);
    }
    else
    {
      unsigned long value = m_match_value->text().toULong(&good, base);

      if (!good)
      {
        m_result_label->setText(tr("Incorrect search value."));
        return;
      }

      val = static_cast<u32>(value);
    }

    switch (GetTypeSize())
    {
    case 2:
      *(u16*)&val = Common::swap16((u8*)&val);
      break;
    case 4:
      val = Common::swap32(val);
      break;
    }
  }

  FilterCheatSearchResults(val, blank_user_value);

  Update();
}

u32 CheatsManager::SwapValue(u32 value)
{
  switch (GetTypeSize())
  {
  case 2:
    *(u16*)&value = Common::swap16((u8*)&value);
    break;
  case 4:
    value = Common::swap32(value);
    break;
  }

  return value;
}

void CheatsManager::TimedUpdate()
{
  if (m_updating)
    return;

  if (m_results.empty())
  {
    m_result_label->clear();
    m_timer->stop();
    m_match_table->setRowCount(0);
    return;
  }

  int results_display;

  if (m_results.size() > MAX_RESULTS)
  {
    results_display = MAX_RESULTS;
    m_result_label->setText(tr("Too many matches to display (%1)").arg(m_results.size()));
  }
  else
  {
    results_display = static_cast<int>(m_results.size());
  }

  m_match_table->setRowCount(results_display);

  int first_row = m_match_table->rowAt(m_match_table->rect().top());
  int last_row = m_match_table->rowAt(m_match_table->rect().bottom());

  if (last_row == -1)
    last_row = m_match_table->rowCount();

  Core::RunAsCPUThread([&] {
    for (int i = first_row; i <= last_row; i++)
    {
      u32 address = m_results[i].address + m_ram.base;
      auto* value_item = new QTableWidgetItem;
      auto* int_item = new QTableWidgetItem;
      auto* float_item = new QTableWidgetItem;

      value_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
      float_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
      int_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

      if (PowerPC::HostIsRAMAddress(address))
      {
        switch (m_search_type_size)
        {
        case 1:
          value_item->setText(
              QStringLiteral("%1").arg(PowerPC::HostRead_U8(address), 2, 16, QLatin1Char('0')));
          break;
        case 2:
          value_item->setText(
              QStringLiteral("%1").arg(PowerPC::HostRead_U16(address), 4, 16, QLatin1Char('0')));
          break;
        case 4:
          value_item->setText(
              QStringLiteral("%1").arg(PowerPC::HostRead_U32(address), 8, 16, QLatin1Char('0')));
          float_item->setText(QString::number(PowerPC::HostRead_F32(address)));
          break;
        default:
          value_item->setText(
              QStringLiteral("%1").arg(PowerPC::HostRead_U32(address), 8, 16, QLatin1Char('0')));
          float_item->setText(QString::number(PowerPC::HostRead_F32(address)));
          break;
        }
      }
      else
      {
        value_item->setText(QStringLiteral("---"));
      }

      bool ok;

      value_item->setData(INDEX_ROLE, static_cast<int>(i));
      float_item->setData(INDEX_ROLE, static_cast<int>(i));
      int_item->setData(INDEX_ROLE, static_cast<int>(i));

      int_item->setText(QString::number(value_item->text().toUInt(&ok, 16)));

      if (!ok)
        int_item->setText(QStringLiteral("-"));

      m_match_table->setItem(static_cast<int>(i), 1, value_item);
      m_match_table->setItem(static_cast<int>(i), 2, int_item);
      m_match_table->setItem(static_cast<int>(i), 3, float_item);
    }
  });
}

void CheatsManager::Update()
{
  // ERROR &Host::UpdateDisasmDialog getting triggered.
  m_updating = true;

  m_match_table->clearContents();

  if (m_results.empty())
  {
    m_result_label->clear();
    m_timer->stop();
    m_match_table->setRowCount(0);
    m_updating = false;
    return;
  }
  else if (m_refresh_enabled->isChecked())
  {
    m_timer->start();
  }

  int results_display = static_cast<int>(m_results.size());

  if (results_display > MAX_RESULTS)
  {
    results_display = MAX_RESULTS;
    m_result_label->setText(tr("Too many matches to display (%1)").arg(m_results.size()));
  }

  m_match_table->setRowCount(results_display);

  m_result_label->setText(tr("%1 Match(es)").arg(m_results.size()));
  m_match_table->setRowCount(static_cast<int>(m_results.size()));

  Core::RunAsCPUThread([&] {
    for (size_t i = 0; i < results_display; i++)
    {
      u32 address = m_results[i].address + m_ram.base;
      auto* address_item =
          new QTableWidgetItem(QStringLiteral("%1").arg(address, 8, 16, QLatin1Char('0')));
      auto* value_item = new QTableWidgetItem;
      auto* int_item = new QTableWidgetItem;
      auto* float_item = new QTableWidgetItem;

      address_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
      value_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
      float_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
      int_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

      if (PowerPC::HostIsRAMAddress(address))
      {
        switch (m_search_type_size)
        {
        case 1:
          value_item->setText(
              QStringLiteral("%1").arg(PowerPC::HostRead_U8(address), 2, 16, QLatin1Char('0')));
          break;
        case 2:
          value_item->setText(
              QStringLiteral("%1").arg(PowerPC::HostRead_U16(address), 4, 16, QLatin1Char('0')));
          break;
        case 4:
          value_item->setText(
              QStringLiteral("%1").arg(PowerPC::HostRead_U32(address), 8, 16, QLatin1Char('0')));
          break;
        default:
          value_item->setText(
              QStringLiteral("%1").arg(PowerPC::HostRead_U32(address), 8, 16, QLatin1Char('0')));
          break;
        }
      }
      else
      {
        value_item->setText(QStringLiteral("---"));
      }
      bool ok;
      address_item->setData(INDEX_ROLE, static_cast<int>(i));
      value_item->setData(INDEX_ROLE, static_cast<int>(i));
      float_item->setData(INDEX_ROLE, static_cast<int>(i));
      int_item->setData(INDEX_ROLE, static_cast<int>(i));

      float_item->setText(QString::number(PowerPC::HostRead_F32(address)));
      int_item->setText(QString::number(value_item->text().toUInt(&ok, 16)));

      if (!ok)
        int_item->setText(QStringLiteral("-"));

      m_match_table->setItem(static_cast<int>(i), 0, address_item);
      m_match_table->setItem(static_cast<int>(i), 1, value_item);
      m_match_table->setItem(static_cast<int>(i), 2, int_item);
      m_match_table->setItem(static_cast<int>(i), 3, float_item);
    }
  });
  m_updating = false;
}

void CheatsManager::OnMatchContextMenu()
{
  QMenu* menu = new QMenu(this);

  menu->addAction(tr("Copy Address"), this, [this] {
    QApplication::clipboard()->setText(m_match_table->selectedItems()[0]->text());
  });
  menu->addAction(tr("Copy Value"), this, [this] {
    QApplication::clipboard()->setText(m_match_table->selectedItems()[1]->text());
  });

  menu->exec(QCursor::pos());
}

void CheatsManager::Reset()
{
  m_results.clear();
  m_match_table->setRowCount(0);
  m_match_next->setEnabled(false);
  for (auto* widget : {m_ram_main, m_ram_wii, m_ram_fakevmem})
    widget->setEnabled(true);
  m_range_start->setEnabled(true);
  m_range_end->setEnabled(true);
  m_match_new->setEnabled(true);
  m_match_table->clearContents();
  m_updating = false;
  m_result_label->setText(QStringLiteral(""));
  Update();
}
