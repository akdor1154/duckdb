# test_replacement_scan.jl

function RangeReplacementScan(info)
    table_name = DuckDB.get_table_name(info)
    number = tryparse(Int64, table_name)
    if number === nothing
        return
    end
    DuckDB.set_function_name(info, "range")
    DuckDB.add_function_parameter(info, DuckDB.create_value(number))
    return
end

@testset "Test replacement scans" begin
    con = DBInterface.connect(DuckDB.DB)

    # add a replacement scan that turns any number provided as a table name into range(X)
    DuckDB.add_replacement_scan!(con, RangeReplacementScan, nothing)

    df = DataFrame(DBInterface.execute(con, "SELECT * FROM \"2\" tbl(a)"))
    @test df.a == [0, 1]

    # this still fails
    @test_throws DuckDB.QueryException DBInterface.execute(con, "SELECT * FROM nonexistant")

    DBInterface.close!(con)
end
