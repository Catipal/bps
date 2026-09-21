// Copyright (c) 2026 The Bitcoin PoS developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_BATCHCOMPOSERDIALOG_H
#define BITCOIN_QT_BATCHCOMPOSERDIALOG_H

#include <qt/sendcoinsrecipient.h>

#include <QDialog>
#include <QList>

class WalletModel;

namespace Ui {
    class BatchComposerDialog;
}

/** Turn a pasted or loaded list of payments into recipients for the Send tab.
 *
 * Paying a lot of people one at a time means clicking Add Recipient and filling
 * two fields for each of them, which is slow and easy to get wrong with no way to
 * check the whole list before sending. This takes the list in one piece, says what
 * is wrong with it line by line, and hands the result to the Send tab so the fee,
 * coin control and confirmation steps are the ones already there.
 */
class BatchComposerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BatchComposerDialog(QWidget *parent, WalletModel *model);
    ~BatchComposerDialog();

    //! Valid only after the dialog was accepted.
    const QList<SendCoinsRecipient>& recipients() const { return m_recipients; }

    //! Most lines anyone pastes deliberately. Past this the wallet would build a
    //! transaction too large to relay, and the tab would spend a long time making
    //! widgets nobody wants.
    static const int MAX_RECIPIENTS = 500;

private Q_SLOTS:
    void on_pasteButton_clicked();
    void on_loadButton_clicked();
    void on_clearButton_clicked();
    void parse();

private:
    //! One parsed line, valid or not.
    struct Row {
        QString address;
        QString label;
        CAmount amount{0};
        QString error;      //!< empty when the line is usable
        QString warning;    //!< usable, but worth a second look
        int line{0};
    };

    QList<Row> parseText(const QString& text) const;
    void showRows(const QList<Row>& rows);

    Ui::BatchComposerDialog *ui;
    WalletModel *model;
    QList<SendCoinsRecipient> m_recipients;
};

#endif // BITCOIN_QT_BATCHCOMPOSERDIALOG_H
