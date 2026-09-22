// Copyright (c) 2026 The Bitcoin PoS developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/batchcomposerdialog.h>
#include <qt/forms/ui_batchcomposerdialog.h>

#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/walletmodel.h>

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QTableWidgetItem>
#include <QTextStream>

BatchComposerDialog::BatchComposerDialog(QWidget *parent, WalletModel *_model) :
    QDialog(parent),
    ui(new Ui::BatchComposerDialog),
    model(_model)
{
    ui->setupUi(this);

    ui->preview->setColumnCount(4);
    ui->preview->setHorizontalHeaderLabels({tr("Line"), tr("Address"), tr("Amount"), tr("Status")});
    // The address and the reason a line was rejected both need room; the reasons are
    // sentences, and elided they say nothing useful.
    ui->preview->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->preview->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    ui->preview->verticalHeader()->setVisible(false);
    ui->preview->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->preview->setSelectionMode(QAbstractItemView::NoSelection);

    connect(ui->input, &QPlainTextEdit::textChanged, this, &BatchComposerDialog::parse);
    parse();
}

BatchComposerDialog::~BatchComposerDialog()
{
    delete ui;
}

void BatchComposerDialog::on_pasteButton_clicked()
{
    const QString text = QApplication::clipboard()->text();
    if (text.isEmpty()) return;
    // Append rather than replace: pasting two lists in a row is a normal thing to do.
    if (!ui->input->toPlainText().isEmpty() && !ui->input->toPlainText().endsWith('\n')) {
        ui->input->appendPlainText(QString());
    }
    ui->input->appendPlainText(text);
}

void BatchComposerDialog::on_loadButton_clicked()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Load payment list"), QString(),
        tr("Text and CSV files (*.txt *.csv);;All files (*)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Load payment list"),
            tr("Could not read %1.").arg(path), QMessageBox::Ok, QMessageBox::Ok);
        return;
    }
    QTextStream in(&file);
    ui->input->setPlainText(in.readAll());
}

void BatchComposerDialog::on_clearButton_clicked()
{
    ui->input->clear();
}

namespace {
//! Addresses never contain any of these, so any of them can separate the fields.
bool hasSeparator(const QString& line)
{
    return line.contains(',') || line.contains(';') || line.contains('\t');
}

QStringList splitOnSeparators(const QString& line)
{
    QString normalised = line;
    normalised.replace(';', ',').replace('\t', ',');
    return normalised.split(',');
}
} // namespace

QList<BatchComposerDialog::Row> BatchComposerDialog::parseText(const QString& text) const
{
    QList<Row> rows;
    if (!model || !model->getOptionsModel()) return rows;
    const int unit = model->getOptionsModel()->getDisplayUnit();

    QSet<QString> seen;
    const QStringList lines = text.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;

        Row row;
        row.line = i + 1;

        QStringList fields;
        if (hasSeparator(line)) {
            fields = splitOnSeparators(line);
        } else {
            // simplified() collapses runs of whitespace, so splitting on a single
            // space cannot produce empty pieces.
            fields = line.simplified().split(' ');
            if (fields.size() != 2) {
                // One field is an address with no amount; three or more can only be a
                // label that was never separated from the rest.
                row.address = line;
                row.error = tr("expected \"address, amount\"");
                rows.append(row);
                continue;
            }
        }
        for (QString& f : fields) f = f.trimmed();

        row.address = fields.value(0);
        const QString amount_str = fields.value(1);
        row.label = fields.mid(2).join(", ").trimmed();

        if (row.address.isEmpty()) {
            row.error = tr("no address");
        } else if (!model->validateAddress(row.address)) {
            row.error = tr("not a valid address");
        } else if (amount_str.isEmpty()) {
            row.error = tr("no amount");
        } else if (!BitcoinUnits::parse(unit, amount_str, &row.amount)) {
            row.error = tr("could not read the amount \"%1\"").arg(amount_str);
        } else if (row.amount <= 0) {
            row.error = tr("amount must be greater than zero");
        } else if (seen.contains(row.address)) {
            row.warning = tr("this address is already in the list");
        }

        if (row.error.isEmpty()) seen.insert(row.address);
        rows.append(row);
    }
    return rows;
}

void BatchComposerDialog::showRows(const QList<Row>& rows)
{
    const int unit = model && model->getOptionsModel()
                        ? model->getOptionsModel()->getDisplayUnit()
                        : BitcoinUnits::BPS;

    ui->preview->setRowCount(rows.size());
    int bad = 0;
    CAmount total = 0;

    for (int i = 0; i < rows.size(); ++i) {
        const Row& row = rows.at(i);
        const bool ok = row.error.isEmpty();
        if (!ok) ++bad; else total += row.amount;

        auto* line = new QTableWidgetItem(QString::number(row.line));
        auto* addr = new QTableWidgetItem(row.label.isEmpty()
                                            ? row.address
                                            : tr("%1  (%2)").arg(row.address, row.label));
        auto* amount = new QTableWidgetItem(ok ? BitcoinUnits::formatWithUnit(unit, row.amount)
                                               : QString());
        auto* status = new QTableWidgetItem(ok ? (row.warning.isEmpty() ? tr("ok") : row.warning)
                                               : row.error);
        amount->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (!ok) {
            const QBrush red(Qt::red);
            addr->setForeground(red);
            status->setForeground(red);
        } else if (!row.warning.isEmpty()) {
            status->setForeground(QBrush(QColor(0xc0, 0x80, 0x00)));
        }

        ui->preview->setItem(i, 0, line);
        ui->preview->setItem(i, 1, addr);
        ui->preview->setItem(i, 2, amount);
        ui->preview->setItem(i, 3, status);
    }
    ui->preview->resizeColumnToContents(0);
    ui->preview->resizeColumnToContents(2);
    ui->preview->setWordWrap(true);

    const int good = rows.size() - bad;
    QString summary;
    if (rows.isEmpty()) {
        summary = tr("Paste one payment per line, as \"address, amount\". A third field "
                     "is used as the label. Lines starting with # are ignored.");
    } else if (bad > 0) {
        summary = tr("%1 of %2 lines cannot be used. Fix them, or delete them, to continue.")
                    .arg(bad).arg(rows.size());
    } else if (good > MAX_RECIPIENTS) {
        summary = tr("%1 recipients is more than one transaction can carry (the limit here "
                     "is %2). Split the list.").arg(good).arg(MAX_RECIPIENTS);
    } else {
        summary = tr("%1 recipients, %2 in total, before the fee.")
                    .arg(good).arg(BitcoinUnits::formatWithUnit(unit, total));
    }
    ui->summary->setText(summary);

    QPushButton* ok_button = ui->buttonBox->button(QDialogButtonBox::Ok);
    if (ok_button) {
        ok_button->setEnabled(good > 0 && bad == 0 && good <= MAX_RECIPIENTS);
        ok_button->setText(tr("Add to the send form"));
    }
}

void BatchComposerDialog::parse()
{
    const QList<Row> rows = parseText(ui->input->toPlainText());
    showRows(rows);

    m_recipients.clear();
    for (const Row& row : rows) {
        if (!row.error.isEmpty()) continue;
        SendCoinsRecipient recipient;
        recipient.address = row.address;
        recipient.label = row.label;
        recipient.amount = row.amount;
        m_recipients.append(recipient);
    }
}
