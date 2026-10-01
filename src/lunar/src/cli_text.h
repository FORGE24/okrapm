#pragma once

#include "okrapmlib/transaction.h"

#include <string>
#include <utility>
#include <vector>

namespace Cli {

/**
 * Error() - 把一条失败写到标准错误。
 * @Message: 失败原因。
 *
 * Return: 无。终端上单词 error 用红色，管道里保持纯文本。
 */
void Error(const std::string &Message);

/**
 * TaskBegin() - 开始一条任务。
 * @Verb: 命令动词，例如 install。
 * @Subject: 任务对象。可以为空。
 *
 * Return: 无。
 */
void TaskBegin(const std::string &Verb, const std::string &Subject = "");

/**
 * TaskOk() - 结束一条成功的任务。
 * @Detail: 结果摘要。
 *
 * Return: 无。
 */
void TaskOk(const std::string &Detail);

/**
 * TaskFail() - 结束一条失败的任务。
 * @Detail: 失败原因。
 *
 * Return: 无。写到标准错误。
 */
void TaskFail(const std::string &Detail);

/**
 * PrintTable() - 打印一张对齐的表。
 * @Headers: 列名。
 * @Rows: 每一行的单元格，列数与 Headers 相同。
 * @Indent: 行首空格数。任务体内的表用 2。
 *
 * Return: 无。没有数据行时在表头下打印 (none)。
 */
void PrintTable(const std::vector<std::string> &Headers,
	const std::vector<std::vector<std::string>> &Rows, int Indent = 0);

/**
 * PrintFields() - 打印两列的字段表。
 * @Rows: 字段名和值。
 *
 * Return: 无。
 */
void PrintFields(const std::vector<std::pair<std::string, std::string>> &Rows);

/**
 * PrintOperations() - 把事务里的操作打成任务体内的表。
 * @Txn: 已经生成的事务。
 *
 * Return: 无。没有操作时不打印表。
 */
void PrintOperations(const okrapm::Transaction &Txn);

/**
 * Help() - 打印 lunar 的命令表。
 *
 * Return: 无。
 */
void Help();

} // namespace Cli
