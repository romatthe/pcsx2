// SPDX-FileCopyrightText: 2002-2024 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DisassemblyWidget.h"

#include "DebugTools/DebugInterface.h"
#include "DebugTools/DisassemblyManager.h"
#include "DebugTools/Breakpoints.h"
#include "DebugTools/MipsAssembler.h"

#include "QtUtils.h"
#include "QtHost.h"
#include <QtGui/QMouseEvent>
#include <QtWidgets/QMenu>
#include <QtGui/QClipboard>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMessageBox>
#include "SymbolTree/NewSymbolDialogs.h"

using namespace QtUtils;

DisassemblyWidget::DisassemblyWidget(QWidget* parent)
	: QWidget(parent)
{
	ui.setupUi(this);

	connect(this, &DisassemblyWidget::customContextMenuRequested, this, &DisassemblyWidget::customMenuRequested);
}

DisassemblyWidget::~DisassemblyWidget() = default;

void DisassemblyWidget::contextCopyAddress()
{
	QGuiApplication::clipboard()->setText(FetchSelectionInfo(SelectionInfo::ADDRESS));
}

void DisassemblyWidget::contextCopyInstructionHex()
{
	QGuiApplication::clipboard()->setText(FetchSelectionInfo(SelectionInfo::INSTRUCTIONHEX));
}

void DisassemblyWidget::contextCopyInstructionText()
{
	QGuiApplication::clipboard()->setText(FetchSelectionInfo(SelectionInfo::INSTRUCTIONTEXT));
}

void DisassemblyWidget::contextAssembleInstruction()
{
	if (!m_cpu->isCpuPaused())
	{
		QMessageBox::warning(this, tr("Assemble Error"), tr("Unable to change assembly while core is running"));
		return;
	}

	DisassemblyLineInfo line;
	bool ok;
	m_disassemblyManager.getLine(m_selectedAddressStart, false, line);
	QString instruction = QInputDialog::getText(this, tr("Assemble Instruction"), "",
		QLineEdit::Normal, QString("%1 %2").arg(line.name.c_str()).arg(line.params.c_str()), &ok);

	if (!ok)
		return;

	u32 encodedInstruction;
	std::string errorText;
	bool valid = MipsAssembleOpcode(instruction.toLocal8Bit().constData(), m_cpu, m_selectedAddressStart, encodedInstruction, errorText);

	if (!valid)
	{
		QMessageBox::warning(this, tr("Assemble Error"), QString::fromStdString(errorText));
		return;
	}
	else
	{
		Host::RunOnCPUThread([this, start = m_selectedAddressStart, end = m_selectedAddressEnd, cpu = m_cpu, val = encodedInstruction] {
			for (u32 i = start; i <= end; i += 4)
			{
				this->m_nopedInstructions.insert({i, cpu->read32(i)});
				cpu->write32(i, val);
			}
			emit VMUpdate();
		});
	}
}

void DisassemblyWidget::contextNoopInstruction()
{
	Host::RunOnCPUThread([this, start = m_selectedAddressStart, end = m_selectedAddressEnd, cpu = m_cpu] {
		for (u32 i = start; i <= end; i += 4)
		{
			this->m_nopedInstructions.insert({i, cpu->read32(i)});
			cpu->write32(i, 0x00);
		}
		emit VMUpdate();
	});
}

void DisassemblyWidget::contextRestoreInstruction()
{
	Host::RunOnCPUThread([this, start = m_selectedAddressStart, end = m_selectedAddressEnd, cpu = m_cpu] {
		for (u32 i = start; i <= end; i += 4)
		{
			if (this->m_nopedInstructions.find(i) != this->m_nopedInstructions.end())
			{
				cpu->write32(i, this->m_nopedInstructions[i]);
				this->m_nopedInstructions.erase(i);
			}
		}
		emit VMUpdate();
	});
}

void DisassemblyWidget::contextRunToCursor()
{
	const u32 selectedAddressStart = m_selectedAddressStart;
	Host::RunOnCPUThread([cpu = m_cpu, selectedAddressStart] {
		CBreakPoints::AddBreakPoint(cpu->getCpuType(), selectedAddressStart, true);
		cpu->resumeCpu();
	});
}

void DisassemblyWidget::contextJumpToCursor()
{
	m_cpu->setPc(m_selectedAddressStart);
	this->repaint();
}

void DisassemblyWidget::contextToggleBreakpoint()
{
	if (!m_cpu->isAlive())
		return;

	const u32 selectedAddressStart = m_selectedAddressStart;
	const BreakPointCpu cpuType = m_cpu->getCpuType();
	if (CBreakPoints::IsAddressBreakPoint(cpuType, selectedAddressStart))
	{
		Host::RunOnCPUThread([cpuType, selectedAddressStart] { CBreakPoints::RemoveBreakPoint(cpuType, selectedAddressStart); });
	}
	else
	{
		Host::RunOnCPUThread([cpuType, selectedAddressStart] { CBreakPoints::AddBreakPoint(cpuType, selectedAddressStart); });
	}

	breakpointsChanged();
	this->repaint();
}

void DisassemblyWidget::contextFollowBranch()
{
	DisassemblyLineInfo line;

	m_disassemblyManager.getLine(m_selectedAddressStart, true, line);

	if (line.type == DISTYPE_OPCODE || line.type == DISTYPE_MACRO)
	{
		if (line.info.isBranch)
			gotoAddressAndSetFocus(line.info.branchTarget);
		else if (line.info.hasRelevantAddress)
			gotoAddressAndSetFocus(line.info.releventAddress);
	}
}

void DisassemblyWidget::contextGoToAddress()
{
	bool ok;
	const QString targetString = QInputDialog::getText(this, tr("Go To In Disassembly"), "",
		QLineEdit::Normal, "", &ok);

	if (!ok)
		return;

	u64 address = 0;
	std::string error;
	if (!m_cpu->evaluateExpression(targetString.toStdString().c_str(), address, error))
	{
		QMessageBox::warning(this, tr("Cannot Go To"), QString::fromStdString(error));
		return;
	}

	gotoAddressAndSetFocus(static_cast<u32>(address) & ~3);
}

void DisassemblyWidget::contextAddFunction()
{
	NewFunctionDialog* dialog = new NewFunctionDialog(*m_cpu, this);
	dialog->setName(QString("func_%1").arg(m_selectedAddressStart, 8, 16, QChar('0')));
	dialog->setAddress(m_selectedAddressStart);
	if (m_selectedAddressEnd != m_selectedAddressStart)
		dialog->setCustomSize(m_selectedAddressEnd - m_selectedAddressStart + 4);
	if (dialog->exec() == QDialog::Accepted)
		update();
}

void DisassemblyWidget::contextCopyFunctionName()
{
	std::string name = m_cpu->GetSymbolGuardian().FunctionStartingAtAddress(m_selectedAddressStart).name;
	QGuiApplication::clipboard()->setText(QString::fromStdString(name));
}

void DisassemblyWidget::contextRemoveFunction()
{
	m_cpu->GetSymbolGuardian().ReadWrite([&](ccc::SymbolDatabase& database) {
		ccc::Function* curFunc = database.functions.symbol_overlapping_address(m_selectedAddressStart);
		if (!curFunc)
			return;

		ccc::Function* previousFunc = database.functions.symbol_overlapping_address(curFunc->address().value - 4);
		if (previousFunc)
			previousFunc->set_size(curFunc->size() + previousFunc->size());

		database.functions.mark_symbol_for_destruction(curFunc->handle(), &database);
		database.destroy_marked_symbols();
	});
}

void DisassemblyWidget::contextRenameFunction()
{
	const FunctionInfo curFunc = m_cpu->GetSymbolGuardian().FunctionOverlappingAddress(m_selectedAddressStart);

	if (!curFunc.address.valid())
	{
		QMessageBox::warning(this, tr("Rename Function Error"), tr("No function / symbol is currently selected."));
		return;
	}

	QString oldName = QString::fromStdString(curFunc.name);

	bool ok;
	QString newName = QInputDialog::getText(this, tr("Rename Function"), tr("Function name"), QLineEdit::Normal, oldName, &ok);
	if (!ok)
		return;

	if (newName.isEmpty())
	{
		QMessageBox::warning(this, tr("Rename Function Error"), tr("Function name cannot be nothing."));
		return;
	}

	m_cpu->GetSymbolGuardian().ReadWrite([&](ccc::SymbolDatabase& database) {
		database.functions.rename_symbol(curFunc.handle, newName.toStdString());
	});
}

void DisassemblyWidget::contextStubFunction()
{
	FunctionInfo function = m_cpu->GetSymbolGuardian().FunctionOverlappingAddress(m_selectedAddressStart);
	u32 address = function.address.valid() ? function.address.value : m_selectedAddressStart;

	Host::RunOnCPUThread([this, address, cpu = m_cpu] {
		this->m_stubbedFunctions.insert({address, {cpu->read32(address), cpu->read32(address + 4)}});
		cpu->write32(address, 0x03E00008); // jr ra
		cpu->write32(address + 4, 0x00000000); // nop
		emit VMUpdate();
	});
}

void DisassemblyWidget::contextRestoreFunction()
{
	u32 address = m_selectedAddressStart;
	m_cpu->GetSymbolGuardian().Read([&](const ccc::SymbolDatabase& database) {
		const ccc::Function* function = database.functions.symbol_overlapping_address(m_selectedAddressStart);
		if (function)
			address = function->address().value;
	});

	auto stub = m_stubbedFunctions.find(address);
	if (stub != m_stubbedFunctions.end())
	{
		Host::RunOnCPUThread([this, address, cpu = m_cpu, stub] {
			auto [first_instruction, second_instruction] = stub->second;
			cpu->write32(address, first_instruction);
			cpu->write32(address + 4, second_instruction);
			this->m_stubbedFunctions.erase(address);
			emit VMUpdate();
		});
	}
	else
	{
		QMessageBox::warning(this, tr("Restore Function Error"), tr("Unable to stub selected address."));
	}
}

void DisassemblyWidget::contextShowOpcode()
{
	m_showInstructionOpcode = !m_showInstructionOpcode;
	this->repaint();
}

void DisassemblyWidget::SetCpu(DebugInterface* cpu)
{
	m_cpu = cpu;
	m_disassemblyManager.setCpu(cpu);
}

QString DisassemblyWidget::GetLineDisasm(u32 address)
{
	DisassemblyLineInfo lineInfo;
	m_disassemblyManager.getLine(address, true, lineInfo);
	return QString("%1 %2").arg(lineInfo.name.c_str()).arg(lineInfo.params.c_str());
};

// Here we go!
void DisassemblyWidget::paintEvent(QPaintEvent* event)
{
	QPainter painter(this);

	const u32 w = painter.device()->width() - 1;
	const u32 h = painter.device()->height() - 1;

	// Get the current font size
	const QFontMetrics fm = painter.fontMetrics();

	// Get the row height
	m_rowHeight = fm.height() + 2;

	// Find the amount of visible rows
	m_visibleRows = h / m_rowHeight;

	m_disassemblyManager.analyze(m_visibleStart, m_disassemblyManager.getNthNextAddress(m_visibleStart, m_visibleRows) - m_visibleStart);

	// Draw the rows
	bool inSelectionBlock = false;
	bool alternate = m_visibleStart % 8;

	const u32 curPC = m_cpu->getPC(); // Get the PC here, because it'll change when we are drawing and make it seem like there are two PCs

	for (u32 i = 0; i <= m_visibleRows; i++)
	{
		const u32 rowAddress = (i * 4) + m_visibleStart;
		// Row backgrounds

		if (inSelectionBlock || (m_selectedAddressStart <= rowAddress && rowAddress <= m_selectedAddressEnd))
		{
			painter.fillRect(0, i * m_rowHeight, w, m_rowHeight, this->palette().highlight());
			inSelectionBlock = m_selectedAddressEnd != rowAddress;
		}
		else
		{
			painter.fillRect(0, i * m_rowHeight, w, m_rowHeight, alternate ? this->palette().base() : this->palette().alternateBase());
		}

		// Row text
		painter.setPen(GetAddressFunctionColor(rowAddress));
		QString lineString = DisassemblyStringFromAddress(rowAddress, painter.font(), curPC, rowAddress == m_selectedAddressStart);

		painter.drawText(2, i * m_rowHeight, w, m_rowHeight, Qt::AlignLeft, lineString);

		// Breakpoint marker
		bool enabled;
		if (CBreakPoints::IsAddressBreakPoint(m_cpu->getCpuType(), rowAddress, &enabled) && !CBreakPoints::IsTempBreakPoint(m_cpu->getCpuType(), rowAddress))
		{
			if (enabled)
			{
				painter.setPen(Qt::green);
				painter.drawText(2, i * m_rowHeight, w, m_rowHeight, Qt::AlignLeft, "\u25A0");
			}
			else
			{
				painter.drawText(2, i * m_rowHeight, w, m_rowHeight, Qt::AlignLeft, "\u2612");
			}
		}
		alternate = !alternate;
	}
	// Draw the branch lines
	// This is where it gets a little scary
	// It's been mostly copied from the wx implementation

	u32 visibleEnd = m_disassemblyManager.getNthNextAddress(m_visibleStart, m_visibleRows);
	std::vector<BranchLine> branchLines = m_disassemblyManager.getBranchLines(m_visibleStart, visibleEnd - m_visibleStart);

	s32 branchCount = 0;
	for (const auto& branchLine : branchLines)
	{
		if (branchCount == (m_showInstructionOpcode ? 3 : 5))
			break;
		const int winBottom = this->height();

		const int x = this->width() - 10 - (branchCount * 10);

		int top, bottom;
		// If the start is technically 'above' our address view
		if (branchLine.first < m_visibleStart)
		{
			top = -1;
		}
		// If the start is technically 'below' our address view
		else if (branchLine.first >= visibleEnd)
		{
			top = winBottom + 1;
		}
		else
		{
			// Explaination
			// ((branchLine.first - m_visibleStart) -> Find the amount of bytes from the top of the view
			// / 4 -> Convert that into rowss in instructions
			// * m_rowHeight -> convert that into rows in pixels
			// + (m_rowHeight / 2) -> Add half a row in pixels to center the arrow
			top = (((branchLine.first - m_visibleStart) / 4) * m_rowHeight) + (m_rowHeight / 2);
		}

		if (branchLine.second < m_visibleStart)
		{
			bottom = -1;
		}
		else if (branchLine.second >= visibleEnd)
		{
			bottom = winBottom + 1;
		}
		else
		{
			bottom = (((branchLine.second - m_visibleStart) / 4) * m_rowHeight) + (m_rowHeight / 2);
		}

		branchCount++;

		if (branchLine.first == m_selectedAddressStart || branchLine.second == m_selectedAddressStart)
		{
			painter.setPen(QColor(0xFF257AFA));
		}
		else
		{
			painter.setPen(QColor(0xFFFF3020));
		}

		if (top < 0) // first is not visible, but second is
		{
			painter.drawLine(x - 2, bottom, x + 2, bottom);
			painter.drawLine(x + 2, bottom, x + 2, 0);

			if (branchLine.type == LINE_DOWN)
			{
				painter.drawLine(x, bottom - 4, x - 4, bottom);
				painter.drawLine(x - 4, bottom, x + 1, bottom + 5);
			}
		}
		else if (bottom > winBottom) // second is not visible, but first is
		{
			painter.drawLine(x - 2, top, x + 2, top);
			painter.drawLine(x + 2, top, x + 2, winBottom);

			if (branchLine.type == LINE_UP)
			{
				painter.drawLine(x, top - 4, x - 4, top);
				painter.drawLine(x - 4, top, x + 1, top + 5);
			}
		}
		else
		{ // both are visible
			if (branchLine.type == LINE_UP)
			{
				painter.drawLine(x - 2, bottom, x + 2, bottom);
				painter.drawLine(x + 2, bottom, x + 2, top);
				painter.drawLine(x + 2, top, x - 4, top);

				painter.drawLine(x, top - 4, x - 4, top);
				painter.drawLine(x - 4, top, x + 1, top + 5);
			}
			else
			{
				painter.drawLine(x - 2, top, x + 2, top);
				painter.drawLine(x + 2, top, x + 2, bottom);
				painter.drawLine(x + 2, bottom, x - 4, bottom);

				painter.drawLine(x, bottom - 4, x - 4, bottom);
				painter.drawLine(x - 4, bottom, x + 1, bottom + 5);
			}
		}
	}
	// Draw a border
	painter.setPen(this->palette().shadow().color());
	painter.drawRect(0, 0, w, h);
}

void DisassemblyWidget::mousePressEvent(QMouseEvent* event)
{
	const u32 selectedAddress = (static_cast<int>(event->position().y()) / m_rowHeight * 4) + m_visibleStart;
	if (event->buttons() & Qt::LeftButton)
	{
		if (event->modifiers() & Qt::ShiftModifier)
		{
			if (selectedAddress < m_selectedAddressStart)
			{
				m_selectedAddressStart = selectedAddress;
			}
			else if (selectedAddress > m_visibleStart)
			{
				m_selectedAddressEnd = selectedAddress;
			}
		}
		else
		{
			m_selectedAddressStart = selectedAddress;
			m_selectedAddressEnd = selectedAddress;
		}
	}
	else if (event->buttons() & Qt::RightButton)
	{
		if (m_selectedAddressStart == m_selectedAddressEnd)
		{
			m_selectedAddressStart = selectedAddress;
			m_selectedAddressEnd = selectedAddress;
		}
	}
	this->repaint();
}

void DisassemblyWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
	if (!m_cpu->isAlive())
		return;

	const u32 selectedAddress = (static_cast<int>(event->position().y()) / m_rowHeight * 4) + m_visibleStart;
	const BreakPointCpu cpuType = m_cpu->getCpuType();
	if (CBreakPoints::IsAddressBreakPoint(cpuType, selectedAddress))
	{
		Host::RunOnCPUThread([cpuType, selectedAddress] { CBreakPoints::RemoveBreakPoint(cpuType, selectedAddress); });
	}
	else
	{
		Host::RunOnCPUThread([cpuType, selectedAddress] { CBreakPoints::AddBreakPoint(cpuType, selectedAddress); });
	}
	breakpointsChanged();
	this->repaint();
}

void DisassemblyWidget::wheelEvent(QWheelEvent* event)
{
	if (event->angleDelta().y() < 0) // todo: max address bounds check?
	{
		m_visibleStart += 4;
	}
	else if (event->angleDelta().y() && m_visibleStart > 0)
	{
		m_visibleStart -= 4;
	}
	this->repaint();
}

void DisassemblyWidget::keyPressEvent(QKeyEvent* event)
{
	switch (event->key())
	{
		case Qt::Key_Up:
		{
			m_selectedAddressStart -= 4;
			if (!(event->modifiers() & Qt::ShiftModifier))
				m_selectedAddressEnd = m_selectedAddressStart;

			// Auto scroll
			if (m_visibleStart > m_selectedAddressStart)
				m_visibleStart -= 4;
		}
		break;
		case Qt::Key_PageUp:
		{
			m_selectedAddressStart -= m_visibleRows * 4;
			m_selectedAddressEnd = m_selectedAddressStart;
			m_visibleStart -= m_visibleRows * 4;
		}
		break;
		case Qt::Key_Down:
		{
			m_selectedAddressEnd += 4;
			if (!(event->modifiers() & Qt::ShiftModifier))
				m_selectedAddressStart = m_selectedAddressEnd;

			// Purposely scroll on the second to last row. It's possible to
			// size the window so part of a row is visible and we don't want to have half a row selected and cut off!
			if (m_visibleStart + ((m_visibleRows - 1) * 4) < m_selectedAddressEnd)
				m_visibleStart += 4;

			break;
		}
		case Qt::Key_PageDown:
		{
			m_selectedAddressStart += m_visibleRows * 4;
			m_selectedAddressEnd = m_selectedAddressStart;
			m_visibleStart += m_visibleRows * 4;
			break;
		}
		case Qt::Key_G:
			contextGoToAddress();
			break;
		case Qt::Key_J:
			contextJumpToCursor();
			break;
		case Qt::Key_C:
			contextCopyInstructionText();
			break;
		case Qt::Key_B:
		case Qt::Key_Space:
			contextToggleBreakpoint();
			break;
		case Qt::Key_M:
			contextAssembleInstruction();
			break;
		case Qt::Key_Right:
			contextFollowBranch();
			break;
		case Qt::Key_Left:
			gotoAddressAndSetFocus(m_cpu->getPC());
			break;
		case Qt::Key_O:
			m_showInstructionOpcode = !m_showInstructionOpcode;
			break;
	}

	this->repaint();
}

void DisassemblyWidget::customMenuRequested(QPoint pos)
{
	if (!m_cpu->isAlive())
		return;

	QMenu* contextMenu = new QMenu(this);

	QAction* action = 0;
	contextMenu->addAction(action = new QAction(tr("Copy Address"), this));
	connect(action, &QAction::triggered, this, &DisassemblyWidget::contextCopyAddress);
	contextMenu->addAction(action = new QAction(tr("Copy Instruction Hex"), this));
	connect(action, &QAction::triggered, this, &DisassemblyWidget::contextCopyInstructionHex);
	contextMenu->addAction(action = new QAction(tr("&Copy Instruction Text"), this));
	action->setShortcut(QKeySequence(Qt::Key_C));
	connect(action, &QAction::triggered, this, &DisassemblyWidget::contextCopyInstructionText);
	if (m_cpu->GetSymbolGuardian().FunctionExistsWithStartingAddress(m_selectedAddressStart))
	{
		contextMenu->addAction(action = new QAction(tr("Copy Function Name"), this));
		connect(action, &QAction::triggered, this, &DisassemblyWidget::contextCopyFunctionName);
	}
	contextMenu->addSeparator();
	if (AddressCanRestore(m_selectedAddressStart, m_selectedAddressEnd))
	{
		contextMenu->addAction(action = new QAction(tr("Restore Instruction(s)"), this));
		connect(action, &QAction::triggered, this, &DisassemblyWidget::contextRestoreInstruction);
	}
	contextMenu->addAction(action = new QAction(tr("Asse&mble new Instruction(s)"), this));
	action->setShortcut(QKeySequence(Qt::Key_M));
	connect(action, &QAction::triggered, this, &DisassemblyWidget::contextAssembleInstruction);
	contextMenu->addAction(action = new QAction(tr("NOP Instruction(s)"), this));
	connect(action, &QAction::triggered, this, &DisassemblyWidget::contextNoopInstruction);
	contextMenu->addSeparator();
	contextMenu->addAction(action = new QAction(tr("Run to Cursor"), this));
	connect(action, &QAction::triggered, this, &DisassemblyWidget::contextRunToCursor);
	contextMenu->addAction(action = new QAction(tr("&Jump to Cursor"), this));
	action->setShortcut(QKeySequence(Qt::Key_J));
	connect(action, &QAction::triggered, this, &DisassemblyWidget::contextJumpToCursor);
	contextMenu->addAction(action = new QAction(tr("Toggle &Breakpoint"), this));
	action->setShortcut(QKeySequence(Qt::Key_B));
	connect(action, &QAction::triggered, this, &DisassemblyWidget::contextToggleBreakpoint);
	contextMenu->addAction(action = new QAction(tr("Follow Branch"), this));
	connect(action, &QAction::triggered, this, &DisassemblyWidget::contextFollowBranch);
	contextMenu->addSeparator();
	contextMenu->addAction(action = new QAction(tr("&Go to Address"), this));
	action->setShortcut(QKeySequence(Qt::Key_G));
	connect(action, &QAction::triggered, this, &DisassemblyWidget::contextGoToAddress);
	contextMenu->addAction(action = new QAction(tr("Go to in Memory View"), this));
	connect(action, &QAction::triggered, this, [this]() { gotoInMemory(m_selectedAddressStart); });
	contextMenu->addSeparator();
	contextMenu->addAction(action = new QAction(tr("Add Function"), this));
	connect(action, &QAction::triggered, this, &DisassemblyWidget::contextAddFunction);
	contextMenu->addAction(action = new QAction(tr("Rename Function"), this));
	connect(action, &QAction::triggered, this, &DisassemblyWidget::contextRenameFunction);
	contextMenu->addAction(action = new QAction(tr("Remove Function"), this));
	connect(action, &QAction::triggered, this, &DisassemblyWidget::contextRemoveFunction);
	if (FunctionCanRestore(m_selectedAddressStart))
	{
		contextMenu->addAction(action = new QAction(tr("Restore Function"), this));
		connect(action, &QAction::triggered, this, &DisassemblyWidget::contextRestoreFunction);
	}
	else
	{
		contextMenu->addAction(action = new QAction(tr("Stub (NOP) Function"), this));
		connect(action, &QAction::triggered, this, &DisassemblyWidget::contextStubFunction);
	}

	contextMenu->addSeparator();
	contextMenu->addAction(action = new QAction(tr("Show &Opcode"), this));
	action->setShortcut(QKeySequence(Qt::Key_O));
	action->setCheckable(true);
	action->setChecked(m_showInstructionOpcode);
	connect(action, &QAction::triggered, this, &DisassemblyWidget::contextShowOpcode);

	contextMenu->setAttribute(Qt::WA_DeleteOnClose);
	contextMenu->popup(this->mapToGlobal(pos));
}

inline QString DisassemblyWidget::DisassemblyStringFromAddress(u32 address, QFont font, u32 pc, bool selected)
{
	DisassemblyLineInfo line;

	if (!m_cpu->isValidAddress(address))
		return tr("%1 NOT VALID ADDRESS").arg(address, 8, 16, QChar('0')).toUpper();
	// Todo? support non symbol view?
	m_disassemblyManager.getLine(address, true, line);

	const bool isConditional = line.info.isConditional && m_cpu->getPC() == address;
	const bool isConditionalMet = line.info.conditionMet;
	const bool isCurrentPC = m_cpu->getPC() == address;

	FunctionInfo function = m_cpu->GetSymbolGuardian().FunctionStartingAtAddress(address);
	SymbolInfo symbol = m_cpu->GetSymbolGuardian().SymbolStartingAtAddress(address);
	const bool showOpcode = m_showInstructionOpcode && m_cpu->isAlive();

	QString lineString;
	if (showOpcode)
	{
		lineString = QString(" %1 %2 %3  %4 %5  %6 %7");
	}
	else
	{
		lineString = QString(" %1 %2  %3 %4  %5 %6");
	}

	if (function.is_no_return)
	{
		lineString = lineString.arg("NR");
	}
	else
	{
		lineString = lineString.arg("  ");
	}

	if (symbol.name.empty())
		lineString = lineString.arg(address, 8, 16, QChar('0')).toUpper();
	else
	{
		QFontMetrics metric(font);
		QString symbolString = QString::fromStdString(symbol.name);

		lineString = lineString.arg(metric.elidedText(symbolString, Qt::ElideRight, (selected ? 32 : 7) * font.pointSize()));
	}

	if (showOpcode)
	{
		const u32 opcode = m_cpu->read32(address);
		lineString = lineString.arg(QtUtils::FilledQStringFromValue(opcode, 16));
	}

	lineString = lineString.leftJustified(4, ' ') // Address / symbol
					 .arg(line.name.c_str())
					 .arg(line.params.c_str()) // opcode + arguments
					 .arg(isConditional ? (isConditionalMet ? "# true" : "# false") : "")
					 .arg(isCurrentPC ? "<--" : "");

	return lineString;
}

QColor DisassemblyWidget::GetAddressFunctionColor(u32 address)
{
	// This is an attempt to figure out if the current palette is dark or light
	// We calculate the luminance of the alternateBase colour
	// and swap between our darker and lighter function colours

	std::array<QColor, 6> colors;
	const QColor base = this->palette().alternateBase().color();

	const auto Y = (base.redF() * 0.33) + (0.5 * base.greenF()) + (0.16 * base.blueF());

	if (Y > 0.5)
	{
		colors = {
			QColor::fromRgba(0xFFFA3434),
			QColor::fromRgba(0xFF206b6b),
			QColor::fromRgba(0xFF858534),
			QColor::fromRgba(0xFF378c37),
			QColor::fromRgba(0xFF783278),
			QColor::fromRgba(0xFF21214a),
		};
	}
	else
	{
		colors = {
			QColor::fromRgba(0xFFe05555),
			QColor::fromRgba(0xFF55e0e0),
			QColor::fromRgba(0xFFe8e855),
			QColor::fromRgba(0xFF55e055),
			QColor::fromRgba(0xFFe055e0),
			QColor::fromRgba(0xFFC2C2F5),
		};
	}

	// Use the address to pick the colour since the value of the handle may
	// change from run to run.
	ccc::Address function_address =
		m_cpu->GetSymbolGuardian().FunctionOverlappingAddress(address).address;
	if (!function_address.valid())
		return palette().text().color();

	// Chop off the first few bits of the address since functions will be
	// aligned in memory.
	return colors[(function_address.value >> 4) % colors.size()];
}

QString DisassemblyWidget::FetchSelectionInfo(SelectionInfo selInfo)
{
	QString infoBlock;
	for (u32 i = m_selectedAddressStart; i <= m_selectedAddressEnd; i += 4)
	{
		if (i != m_selectedAddressStart)
			infoBlock += '\n';
		if (selInfo == SelectionInfo::ADDRESS)
		{
			infoBlock += FilledQStringFromValue(i, 16);
		}
		else if (selInfo == SelectionInfo::INSTRUCTIONTEXT)
		{
			DisassemblyLineInfo line;
			m_disassemblyManager.getLine(i, true, line);
			infoBlock += QString("%1 %2").arg(line.name.c_str()).arg(line.params.c_str());
		}
		else // INSTRUCTIONHEX
		{
			infoBlock += FilledQStringFromValue(m_cpu->read32(i), 16);
		}
	}
	return infoBlock;
}

void DisassemblyWidget::gotoAddressAndSetFocus(u32 address)
{
	gotoAddress(address, true);
}

void DisassemblyWidget::gotoAddress(u32 address, bool should_set_focus)
{
	const u32 destAddress = address & ~3;
	// Center the address
	m_visibleStart = (destAddress - (m_visibleRows * 4 / 2)) & ~3;
	m_selectedAddressStart = destAddress;
	m_selectedAddressEnd = destAddress;

	this->repaint();
	if (should_set_focus)
		this->setFocus();
}

bool DisassemblyWidget::AddressCanRestore(u32 start, u32 end)
{
	for (u32 i = start; i <= end; i += 4)
	{
		if (this->m_nopedInstructions.find(i) != this->m_nopedInstructions.end())
		{
			return true;
		}
	}
	return false;
}

bool DisassemblyWidget::FunctionCanRestore(u32 address)
{
	FunctionInfo function = m_cpu->GetSymbolGuardian().FunctionOverlappingAddress(address);
	if (function.address.valid())
		address = function.address.value;

	return m_stubbedFunctions.find(address) != m_stubbedFunctions.end();
}
