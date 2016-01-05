#include "authorpendingtablemodel.h"

#include <QDialog>
#include <QHBoxLayout>
#include <QFont>
#include <QBrush>

AuthorPendingTableModel::AuthorPendingTableModel(QObject *parent) :
    QAbstractTableModel(parent)
{
}

int AuthorPendingTableModel::rowCount(const QModelIndex & /*parent*/) const
{
   return pending.size();
}

int AuthorPendingTableModel::columnCount(const QModelIndex & /*parent*/) const
{
    return 3;
}

QVariant AuthorPendingTableModel::data(const QModelIndex &index, int role) const
{
    int row = index.row();
    int col = index.column();

    switch(role) {
    case Qt::DisplayRole:
    {
        json_spirit::Array pendingCreation = pending.at(row);

        // Type
        if (col == 0) {
            std::string type = json_spirit::write_string(pendingCreation.back(), true);
            return QString::fromStdString(type);
        }

        // Address
        if (col == 1) {
            std::string address = json_spirit::write_string(pendingCreation.front(), true);
            return QString::fromStdString(address);
        }

        // Fee
        if (col == 2) {
            return "0.0"; // Dummy temp
         }

        break;
    }
    case Qt::FontRole:
        if (col == 0) // Type is bold
        {
            QFont boldFont;
            boldFont.setBold(true);
            return boldFont;
        }
        break;
    case Qt::BackgroundRole:
        break;
    case Qt::TextAlignmentRole:
        break;
    case Qt::CheckStateRole:
        break;
    }

    return QVariant();
}

QVariant AuthorPendingTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role == Qt::DisplayRole)
    {
        if (orientation == Qt::Horizontal) {
            switch (section)
            {
            case 0:
                return QString("Type");
            case 1:
                return QString("Address");
            case 2:
                return QString("Fee");
            }
        }
    }
    return QVariant();
}

void AuthorPendingTableModel::editCombo(const QModelIndex &index)
{
    comboCreationWidget = new ComboCreationWidget;

    QHBoxLayout *hbox = new QHBoxLayout;
    hbox->addWidget(comboCreationWidget);

    QDialog *dialog = new QDialog;
    dialog->setLayout(hbox);
    dialog->show();
}

void AuthorPendingTableModel::editDecision(const QModelIndex &index)
{
    DecisionCreationWidget *creationWidget = new DecisionCreationWidget;
    QHBoxLayout *hbox = new QHBoxLayout;
    hbox->addWidget(creationWidget);

    QDialog *dialog = new QDialog;
    dialog->setLayout(hbox);
    dialog->show();
}

void AuthorPendingTableModel::editDecisionMarket(const QModelIndex &index)
{
    DecisionMarketCreationWidget *marketCreationWidget = new DecisionMarketCreationWidget;
    QHBoxLayout *hbox = new QHBoxLayout;
    hbox->addWidget(marketCreationWidget);

    QDialog *dialog = new QDialog;
    dialog->setLayout(hbox);
    dialog->show();
}

void AuthorPendingTableModel::on_tableView_doubleClicked(const QModelIndex &index)
{
    // Figure out what we are editing, and open proper dialog
    json_spirit::Array itemClicked = pending.at(index.row());
    std::string itemType = json_spirit::write_string(itemClicked.back(), true);
    std::cout << "Item Type:" << itemType << std::endl;

    if (itemType == "\"combo\"") {
        editCombo(index);
    } else if (itemType == "\"decision\"") {
        editDecision(index);
    } else if (itemType == "\"market\"") {
        editDecisionMarket(index);
    } else {
        return;
    }
}

void AuthorPendingTableModel::receivePendingCombo(json_spirit::Array array)
{
    pending.push_back(array);
}

void AuthorPendingTableModel::receivePendingDecision(json_spirit::Array array)
{
    int row = pending.size();
    beginInsertRows(QModelIndex(), row, row);

    pending.push_back(array);

    endInsertRows();
}

void AuthorPendingTableModel::receivePendingDecisionMarket(json_spirit::Array array)
{
    int row = pending.size();
    beginInsertRows(QModelIndex(), row, row);

    pending.push_back(array);

    endInsertRows();
}
