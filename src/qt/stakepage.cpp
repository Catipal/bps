// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/stakepage.h>
#include <qt/forms/ui_stakepage.h>

#include <qt/addresstablemodel.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactiontablemodel.h>
#include <qt/receiverequestdialog.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/walletmodel.h>
#include <interfaces/wallet.h>
#include <interfaces/node.h>
#include <qt/transactiondescdialog.h>
#include <qt/transactionview.h>
#include <qt/sendcoinsdialog.h>
#include <amount.h>
#include <miner.h>
#include <wallet/wallet.h>

#include <QSortFilterProxyModel>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QMessageBox>
#include <QSignalBlocker>

Q_DECLARE_METATYPE(interfaces::WalletBalances)

StakePage::StakePage(const PlatformStyle *_platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::StakePage),
    clientModel(nullptr),
    walletModel(nullptr),
    platformStyle(_platformStyle),
    transactionView(nullptr),
    receivedDelegationsView(nullptr),
    myDelegationsView(nullptr),
    sendCoinsDialog(nullptr),
    m_subsidy(0),
    m_networkWeight(0),
    m_expectedAnnualROI(0)
{
    ui->setupUi(this);

    if (!_platformStyle->getImagesOnButtons()) {
        ui->newAddressButton->setIcon(QIcon());
    } else {
        ui->newAddressButton->setIcon(_platformStyle->SingleColorIcon(":/icons/receiving_addresses"));
    }

    ui->checkStake->setEnabled(gArgs.GetBoolArg("-staking", DEFAULT_STAKE));
    transactionView = new TransactionView(platformStyle, this, true);
    ui->frameStakeRecords->layout()->addWidget(transactionView);

    receivedDelegationsView = new TransactionView(platformStyle, this, true);
    ui->frameTabReceivedDelegations->layout()->addWidget(receivedDelegationsView);
    myDelegationsView = new TransactionView(platformStyle, this, true, true);
    ui->frameTabMyDelegations->layout()->addWidget(myDelegationsView);
    sendCoinsDialog = new SendCoinsDialog(platformStyle, nullptr, true);
    ui->pageManualDelegation->layout()->addWidget(sendCoinsDialog);

    ui->autoStakerPasteButton->setIcon(platformStyle->SingleColorIcon(":/icons/editpaste"));
    GUIUtil::setupAddressWidget(ui->autoStakerAddress, this);
}

StakePage::~StakePage()
{
    delete ui;
    delete transactionView;
    delete receivedDelegationsView;
    delete myDelegationsView;
    delete sendCoinsDialog;
}

void StakePage::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;

    if (_clientModel) {
        connect(_clientModel, &ClientModel::numBlocksChanged, this, &StakePage::numBlocksChanged);
        int height = _clientModel->node().getNumBlocks();
        ui->labelHeight->setText(QString::number(height));
        m_subsidy = _clientModel->node().getBlockSubsidy(height);
        m_networkWeight = _clientModel->node().getNetworkStakeWeight();
        m_expectedAnnualROI = _clientModel->node().getEstimatedAnnualROI();
        updateNetworkWeight();
        updateAnnualROI();
    }

    this->sendCoinsDialog->setClientModel(_clientModel);
}

void StakePage::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
    if(model && model->getOptionsModel())
    {
        transactionView->setModel(model);
        transactionView->chooseType(4); // Mined blocks

        receivedDelegationsView->setModel(model);
        receivedDelegationsView->chooseType(8); // The staker sees received delegations

        myDelegationsView->setModel(model);
        myDelegationsView->chooseType(7); // My delegations to others

        ui->checkStake->setChecked(model->wallet().getEnabledStaking());
        syncAutoDelegation();
        // Nothing was watching this before, so the switch never noticed the wallet
        // being encrypted or locked from anywhere else in the UI.
        connect(model, &WalletModel::encryptionStatusChanged, this, &StakePage::updateEncryptionStatus);
        // Set the button to be enabled or disabled based on whether the wallet can give out new addresses.
        ui->newAddressButton->setEnabled(model->wallet().canGetAddresses());

        // Keep up to date with wallet
        interfaces::Wallet& wallet = model->wallet();
        interfaces::WalletBalances balances = wallet.getBalances();
        setBalance(balances);
        connect(model, &WalletModel::balanceChanged, this, &StakePage::setBalance);

        // Re-emit coinsSent signal from sendCoinsDialog.
        connect(sendCoinsDialog, &SendCoinsDialog::coinsSent, this, &StakePage::coinsSent);

        // Re-emit message signal from sendCoinsDialog.
        connect(sendCoinsDialog, &SendCoinsDialog::message, this, &StakePage::message);

        connect(model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &StakePage::updateDisplayUnit);

        // Enable/disable the receive button if the wallet is now able/unable to give out new addresses.
        connect(model, &WalletModel::canGetAddressesChanged, [this] {
            ui->newAddressButton->setEnabled(walletModel->wallet().canGetAddresses());
        });

        connect(model->getOptionsModel(), &OptionsModel::coldStakerControlFeaturesChanged, this, &StakePage::coldStakerControlFeaturesChanged);
        coldStakerControlFeaturesChanged(model->getOptionsModel()->getColdStakerControlFeatures());
    }

    this->sendCoinsDialog->setModel(model);

    // update the display unit, to not use the default ("BPS")
    updateDisplayUnit();
}

void StakePage::setBalance(const interfaces::WalletBalances& balances)
{
    int unit = walletModel->getOptionsModel()->getDisplayUnit();
    m_balances = balances;
    ui->labelAssets->setText(BitcoinUnits::formatWithUnit(unit, balances.stakeable, false, BitcoinUnits::SeparatorStyle::ALWAYS));
    ui->labelOwnedAssets->setText(BitcoinUnits::formatWithUnit(unit, balances.stakeable - balances.stakeable_delegations, false, BitcoinUnits::SeparatorStyle::ALWAYS));
    ui->labelDelegations->setText(BitcoinUnits::formatWithUnit(unit, balances.stakeable_delegations, false, BitcoinUnits::SeparatorStyle::ALWAYS));
    ui->labelAssetsImmature->setText(BitcoinUnits::formatWithUnit(unit, balances.immature_stakeable, false, BitcoinUnits::SeparatorStyle::ALWAYS));
    ui->labelAssetsImmature->setVisible(balances.immature_stakeable != 0);
    ui->labelAssetsImmatureText->setVisible(balances.immature_stakeable != 0);
    ui->labelStake->setText(BitcoinUnits::formatWithUnit(unit, balances.stake, false, BitcoinUnits::SeparatorStyle::ALWAYS));
    ui->labelDelegatedAssets->setText(BitcoinUnits::formatWithUnit(unit, balances.delegated + balances.immature_delegated, false, BitcoinUnits::SeparatorStyle::ALWAYS));
}

void StakePage::on_checkStake_clicked(bool checked)
{
    if(!walletModel)
        return;

    if(checked && WalletModel::Locked == walletModel->getEncryptionStatus())
    {
        // Ask first, enable only once there is a key to stake with. This used to set
        // the flag before prompting, so cancelling the passphrase dialog left an
        // encrypted, locked wallet with staking switched on: the staker thread ran and
        // could not sign anything. onUnlockFinished() picks it up from here.
        Q_EMIT requireUnlock(true);
        return;
    }

    walletModel->wallet().setEnabledStaking(checked);
    syncStakingSwitch();
}

void StakePage::onUnlockFinished(bool unlocked)
{
    if(!walletModel)
        return;

    if(unlocked)
        walletModel->wallet().setEnabledStaking(true);

    syncStakingSwitch();
}

void StakePage::syncStakingSwitch()
{
    if(!walletModel)
        return;

    // Drive the switch from what the wallet actually has, never the other way round.
    // The old code called checkStake->onStatusChanged(), which is the *click* handler
    // (`bool checked = !isChecked()`) -- it animated the switch to the opposite of
    // wherever it happened to be and never touched m_enabled_staking, so after a
    // cancelled unlock the widget and the wallet disagreed permanently.
    const bool enabled = walletModel->wallet().getEnabledStaking();
    if(ui->checkStake->isChecked() != enabled)
    {
        const QSignalBlocker blocker(ui->checkStake);
        ui->checkStake->setChecked(enabled);
    }
}

void StakePage::updateDisplayUnit()
{
    if(walletModel && walletModel->getOptionsModel())
    {
        if (m_balances.balance != -1) {
            setBalance(m_balances);
        }
        updateSubsidy();
    }
}

void StakePage::numBlocksChanged(int count, const QDateTime &, double, bool headers)
{
    if(!headers && clientModel && walletModel)
    {
        ui->labelHeight->setText(BitcoinUnits::formatInt(count));
        m_subsidy = clientModel->node().getBlockSubsidy(count);
        m_networkWeight = clientModel->node().getNetworkStakeWeight();
        m_expectedAnnualROI = clientModel->node().getEstimatedAnnualROI();
        updateSubsidy();
        updateNetworkWeight();
        updateAnnualROI();
    }
}

void StakePage::updateSubsidy()
{
    int unit = walletModel->getOptionsModel()->getDisplayUnit();
    QString strSubsidy = BitcoinUnits::formatWithUnit(unit, m_subsidy, false, BitcoinUnits::SeparatorStyle::ALWAYS) + "/Block";
    ui->labelReward->setText(strSubsidy);
}

void StakePage::updateNetworkWeight()
{
    ui->labelWeight->setText(BitcoinUnits::formatInt(m_networkWeight / COIN));
}

void StakePage::updateAnnualROI()
{
    ui->labelROI->setText(QString::number(m_expectedAnnualROI, 'f', 2) + "%");
}

void StakePage::updateEncryptionStatus()
{
    if(!walletModel)
        return;

    // Deliberately does not stop staking when the wallet locks. ThreadStakeMiner
    // already waits while `IsLocked() || !m_enabled_staking` and picks straight back up
    // on unlock, so there is nothing to stop -- and stopping it was destructive: it
    // joined the thread, and nothing started it again. Locking the wallet for a moment,
    // which is exactly what the passphrase prompt behind Send does, killed staking for
    // the rest of the session.
    //
    // So the switch shows whether staking is enabled, not whether it is signing right
    // now. getstakingstatus already separates those: walletstakingenabled against
    // staking.
    syncStakingSwitch();
}

void StakePage::on_newAddressButton_clicked()
{
    if(!walletModel || !walletModel->getOptionsModel() || !walletModel->getAddressTableModel() || !walletModel->getRecentRequestsTableModel())
        return;

    QString address;
    QString label = ui->newAddressLabel->text();
    /* Generate new staking address */
    OutputType address_type = OutputType::LEGACY;
    address = walletModel->getAddressTableModel()->addRow(AddressTableModel::Receive, label, "", address_type);

    switch(walletModel->getAddressTableModel()->getEditStatus())
    {
    case AddressTableModel::EditStatus::OK: {
        // Success
        SendCoinsRecipient info(address, label, 0, "");
        ReceiveRequestDialog *dialog = new ReceiveRequestDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setModel(walletModel);
        dialog->setInfo(info);
        dialog->show();
        break;
    }
    case AddressTableModel::EditStatus::WALLET_UNLOCK_FAILURE:
        QMessageBox::critical(this, windowTitle(),
            tr("Could not unlock wallet."),
            QMessageBox::Ok, QMessageBox::Ok);
        break;
    case AddressTableModel::EditStatus::KEY_GENERATION_FAILURE:
        QMessageBox::critical(this, windowTitle(),
            tr("Could not generate new %1 address").arg(QString::fromStdString(FormatOutputType(address_type))),
            QMessageBox::Ok, QMessageBox::Ok);
        break;
    // These aren't valid return values for our action
    case AddressTableModel::EditStatus::INVALID_ADDRESS:
    case AddressTableModel::EditStatus::DUPLICATE_ADDRESS:
    case AddressTableModel::EditStatus::NO_CHANGES:
        assert(false);
    }
    ui->newAddressLabel->setText("");
}

void StakePage::syncAutoDelegation()
{
    if (!walletModel) return;

    const QString staker = QString::fromStdString(walletModel->wallet().getStakingAddress());
    const bool on = walletModel->wallet().isAutoDelegating();
    ui->autoStakerAddress->setText(staker);
    ui->autoDelegationClearButton->setEnabled(!staker.isEmpty());

    if (on) {
        ui->labelAutoDelegationStatus->setText(
            tr("On. Coins received at this wallet's delegation addresses stake automatically, "
               "and change stays delegated."));
    } else if (!staker.isEmpty()) {
        // The record is there but cold staking is switched off for the node, so the
        // outputs would not be stakeable. Saying "on" here would be a lie.
        ui->labelAutoDelegationStatus->setText(
            tr("A staking address is set, but cold staking is disabled on this node "
               "(-coldstaking=0), so nothing is being delegated."));
    } else {
        ui->labelAutoDelegationStatus->setText(
            tr("Off. Delegation addresses are unavailable and change goes to ordinary "
               "addresses, which do not stake."));
    }
}

void StakePage::on_delegationModeSelector_currentIndexChanged(int index)
{
    ui->delegationModeStack->setCurrentIndex(index);
    if (index == 1) syncAutoDelegation();
}

void StakePage::on_autoStakerPasteButton_clicked()
{
    ui->autoStakerAddress->setText(QApplication::clipboard()->text());
}

void StakePage::on_autoDelegationApplyButton_clicked()
{
    if (!walletModel) return;

    const QString staker = ui->autoStakerAddress->text().trimmed();
    if (!walletModel->validateAddress(staker)) {
        ui->autoStakerAddress->setValid(false);
        return;
    }

    if (!walletModel->wallet().setStakingAddress(staker.toStdString())) {
        // The address is valid but not one a P2CS script can name, or the record could
        // not be written. Either way nothing changed, so say so rather than leaving the
        // page claiming it is on.
        QMessageBox::warning(this, tr("Automatic delegation"),
            tr("Could not use that address. A delegation commits to a key hash, so the "
               "staking address has to be a legacy address."),
            QMessageBox::Ok, QMessageBox::Ok);
        syncAutoDelegation();
        return;
    }
    syncAutoDelegation();
}

void StakePage::on_autoDelegationClearButton_clicked()
{
    if (!walletModel) return;
    // Coins already delegated stay delegated; this only stops new ones being made.
    walletModel->wallet().setStakingAddress("");
    syncAutoDelegation();
}

// ColdStake Control: settings menu - cold staker control enabled/disabled by user
void StakePage::coldStakerControlFeaturesChanged(bool checked)
{
    int index = ui->tabWidget->indexOf(ui->tab_received_delegations);

    if (checked && index == -1)
    {
        ui->tabWidget->addTab(ui->tab_received_delegations, "Received Delegations");
    }
    else if (!checked && index != -1)
    {
        ui->tabWidget->removeTab(index);
    }
}
